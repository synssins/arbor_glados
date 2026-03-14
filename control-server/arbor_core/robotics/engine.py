"""
Kinematics engine — the central motion dispatch layer.

Accepts high-level motion commands (joint moves, Cartesian poses,
velocity commands), runs the appropriate kinematic solver, and
dispatches resulting joint values to actuator backends.

Phase 1: Joint-space moves only (direct dispatch).
Phase 2: Vehicle kinematics (differential, ackermann, mecanum).
Phase 3: Arm IK/FK (serial chain, SCARA).

Task: Robotics Phase 1
"""

from __future__ import annotations

import time
from typing import TYPE_CHECKING, Any

import structlog

from arbor_core.robotics.models import (
    ActuatorBackendType,
    CartesianPose,
    RobotState,
    RobotType,
    VelocityCommand,
)

if TYPE_CHECKING:
    from arbor_core.events import EventBus
    from arbor_core.robotics.backends.klipper_backend import KlipperBackend
    from arbor_core.robotics.backends.servo_backend import ArborServoBackend
    from arbor_core.robotics.registry import RobotRegistry

logger = structlog.get_logger(__name__)


class EngineError(Exception):
    """Raised when a kinematics engine operation fails."""


class KinematicsEngine:
    """
    Central motion dispatch engine.

    Coordinates between robot profiles, kinematic solvers, and
    actuator backends to execute motion commands.

    Usage::

        engine = KinematicsEngine(registry, backends)
        await engine.move_joints("arm-01", {"base": 1.57, "shoulder": 0.78})
        await engine.drive("tank", VelocityCommand(linear_x=0.5))
    """

    def __init__(
        self,
        registry: RobotRegistry,
        backends: dict[str, ArborServoBackend | KlipperBackend],
        event_bus: EventBus | None = None,
    ) -> None:
        """
        Args:
            registry: Robot profile registry.
            backends: Dict of backend_type_name -> backend instance.
                      Keys: "arbor_servo", "klipper"
            event_bus: Optional event bus for publishing motion events.
        """
        self._registry = registry
        self._backends = backends
        self._event_bus = event_bus
        # Track last-known joint positions per robot
        self._joint_states: dict[str, dict[str, float]] = {}

    def _get_backend(
        self, backend_type: ActuatorBackendType
    ) -> Any:
        """
        Get the actuator backend for a given type.

        Raises:
            EngineError: If the backend type is not registered.
        """
        # Map enum values to backend dict keys
        key_map = {
            ActuatorBackendType.ARBOR_SERVO: "arbor_servo",
            ActuatorBackendType.KLIPPER_STEPPER: "klipper",
            ActuatorBackendType.KLIPPER_SERVO: "klipper",
        }
        key = key_map.get(backend_type)
        if key is None or key not in self._backends:
            msg = f"No backend registered for type '{backend_type}'"
            raise EngineError(msg)
        return self._backends[key]

    def _joint_to_hw(
        self, joint_value: float, scale: float, offset: float
    ) -> float:
        """Convert joint-space value to hardware value: hw = joint * scale + offset."""
        return joint_value * scale + offset

    def _hw_to_joint(
        self, hw_value: float, scale: float, offset: float
    ) -> float:
        """Convert hardware value to joint-space: joint = (hw - offset) / scale."""
        if scale == 0:
            return 0.0
        return (hw_value - offset) / scale

    async def move_joints(
        self,
        robot_id: str,
        joint_values: dict[str, float],
        speed: float | None = None,
    ) -> dict[str, Any]:
        """
        Move specific joints to target positions.

        Validates joint limits, applies scale/offset, dispatches
        to the appropriate backend for each joint.

        Args:
            robot_id: Robot profile ID.
            joint_values: Dict of joint_name -> target value (radians/meters).
            speed: Optional move speed.

        Returns:
            Dict with status and per-joint results.

        Raises:
            EngineError: If robot not found or joint validation fails.
        """
        from arbor_core.robotics.registry import RegistryError

        try:
            profile = self._registry.get(robot_id)
        except RegistryError as exc:
            raise EngineError(str(exc)) from exc

        # Build joint lookup
        joint_map = {j.name: j for j in profile.joints}

        # Validate joint names
        unknown = set(joint_values.keys()) - set(joint_map.keys())
        if unknown:
            msg = f"Unknown joints for robot '{robot_id}': {unknown}"
            raise EngineError(msg)

        results: dict[str, Any] = {}
        errors: list[str] = []

        for joint_name, target_value in joint_values.items():
            joint_cfg = joint_map[joint_name]

            # Validate limits
            if joint_cfg.min_value is not None and target_value < joint_cfg.min_value:
                errors.append(
                    f"{joint_name}: {target_value} below min {joint_cfg.min_value}"
                )
                continue
            if joint_cfg.max_value is not None and target_value > joint_cfg.max_value:
                errors.append(
                    f"{joint_name}: {target_value} above max {joint_cfg.max_value}"
                )
                continue

            # Convert to hardware value
            hw_value = self._joint_to_hw(
                target_value,
                joint_cfg.actuator.scale,
                joint_cfg.actuator.offset,
            )

            # Get backend and dispatch
            try:
                backend = self._get_backend(joint_cfg.actuator.backend)
                actuator_name = self._actuator_name(joint_cfg)

                is_servo = (
                    joint_cfg.actuator.backend == ActuatorBackendType.KLIPPER_SERVO
                )

                result = await backend.send_position(
                    actuator_name,
                    hw_value,
                    speed=speed,
                    is_servo=is_servo,
                )
                results[joint_name] = {"status": "ok", "hw_value": hw_value}

                # Update state tracking
                if robot_id not in self._joint_states:
                    self._joint_states[robot_id] = {}
                self._joint_states[robot_id][joint_name] = target_value

            except Exception as exc:
                errors.append(f"{joint_name}: {exc}")
                results[joint_name] = {"status": "error", "error": str(exc)}

        status = "ok" if not errors else "partial" if results else "error"
        response = {
            "status": status,
            "robot_id": robot_id,
            "results": results,
        }
        if errors:
            response["errors"] = errors

        logger.info(
            "joints_moved",
            robot_id=robot_id,
            status=status,
            joints=list(joint_values.keys()),
        )

        return response

    async def move_cartesian(
        self,
        robot_id: str,
        pose: CartesianPose,
    ) -> dict[str, Any]:
        """
        Move end-effector to a Cartesian pose using IK.

        Runs the appropriate IK solver (serial chain or SCARA),
        then dispatches computed joint angles via move_joints().

        Args:
            robot_id: Robot profile ID.
            pose: Target 6DOF pose.
        """
        from arbor_core.robotics.registry import RegistryError

        try:
            profile = self._registry.get(robot_id)
        except RegistryError as exc:
            raise EngineError(str(exc)) from exc

        if profile.robot_type not in (RobotType.SERIAL_ARM, RobotType.SCARA):
            msg = f"Cartesian move not supported for robot type '{profile.robot_type}'"
            raise EngineError(msg)

        # Get current angles as initial guess for IK
        current_state = self._joint_states.get(robot_id, {})
        current_angles = [
            current_state.get(j.name, j.home_value) for j in profile.joints
        ]

        try:
            if profile.robot_type == RobotType.SERIAL_ARM:
                from arbor_core.robotics.arms.serial_chain import (
                    SerialChainSolver,
                    SerialChainError,
                )

                solver = SerialChainSolver(
                    dh_parameters=profile.dh_parameters,
                    joints=profile.joints,
                    name=robot_id,
                )
                joint_angles = solver.inverse_kinematics(
                    target_pose=pose,
                    initial_angles=current_angles,
                )
                joint_names = solver.joint_names()

            elif profile.robot_type == RobotType.SCARA:
                from arbor_core.robotics.arms.scara import (
                    SCARASolver,
                    SCARAError,
                )

                solver_s = SCARASolver(
                    dh_parameters=profile.dh_parameters,
                    joints=profile.joints,
                    name=robot_id,
                )
                joint_angles = solver_s.inverse_kinematics(
                    target_pose=pose,
                )
                joint_names = solver_s.joint_names()
            else:
                msg = f"No IK solver for type '{profile.robot_type}'"
                raise EngineError(msg)

        except (ImportError, Exception) as exc:
            # Handle ikpy not installed or IK failure
            error_msg = str(exc)
            if "ikpy" in error_msg or "numpy" in error_msg:
                return {
                    "status": "error",
                    "message": (
                        "IK solver dependencies not installed. "
                        "Install ikpy and numpy: pip install ikpy numpy"
                    ),
                    "robot_id": robot_id,
                }
            raise EngineError(error_msg) from exc

        # Build joint value dict and dispatch
        joint_values = dict(zip(joint_names, joint_angles))

        logger.info(
            "ik_computed",
            robot_id=robot_id,
            solver=profile.robot_type.value,
            target=[pose.x, pose.y, pose.z],
            angles=joint_angles,
        )

        return await self.move_joints(robot_id, joint_values)

    async def drive(
        self,
        robot_id: str,
        cmd: VelocityCommand,
    ) -> dict[str, Any]:
        """
        Execute a vehicle velocity command.

        Runs the appropriate vehicle kinematics solver to convert
        body-frame velocity into wheel/steering commands, then
        dispatches to backends via send_velocity or send_position.

        Args:
            robot_id: Robot profile ID.
            cmd: Body-frame velocity command.
        """
        from arbor_core.robotics.registry import RegistryError
        from arbor_core.robotics.vehicles.ackermann import AckermannSolver
        from arbor_core.robotics.vehicles.differential import DifferentialDriveSolver
        from arbor_core.robotics.vehicles.mecanum import MecanumSolver

        try:
            profile = self._registry.get(robot_id)
        except RegistryError as exc:
            raise EngineError(str(exc)) from exc

        vehicle_types = {
            RobotType.DIFFERENTIAL,
            RobotType.ACKERMANN,
            RobotType.MECANUM,
        }
        if profile.robot_type not in vehicle_types:
            msg = f"Drive command not supported for robot type '{profile.robot_type}'"
            raise EngineError(msg)

        if profile.vehicle_geometry is None:
            msg = f"Robot '{robot_id}' has no vehicle_geometry configured"
            raise EngineError(msg)

        # Select and run the appropriate solver
        if profile.robot_type == RobotType.DIFFERENTIAL:
            solver = DifferentialDriveSolver(profile.vehicle_geometry)
            wheel_values = solver.to_joint_values(cmd)
        elif profile.robot_type == RobotType.ACKERMANN:
            solver_a = AckermannSolver(profile.vehicle_geometry)
            wheel_values = solver_a.to_joint_values(cmd)
        elif profile.robot_type == RobotType.MECANUM:
            solver_m = MecanumSolver(profile.vehicle_geometry)
            wheel_values = solver_m.to_joint_values(cmd)
        else:
            msg = f"No solver for robot type '{profile.robot_type}'"
            raise EngineError(msg)

        # Dispatch wheel commands to backends
        joint_map = {j.name: j for j in profile.joints}
        results: dict[str, Any] = {}
        errors: list[str] = []

        for joint_name, value in wheel_values.items():
            joint_cfg = joint_map.get(joint_name)
            if joint_cfg is None:
                errors.append(
                    f"Solver output '{joint_name}' not found in robot joints"
                )
                continue

            # Apply scale/offset
            hw_value = self._joint_to_hw(
                value, joint_cfg.actuator.scale, joint_cfg.actuator.offset
            )

            try:
                backend = self._get_backend(joint_cfg.actuator.backend)
                actuator_name = self._actuator_name(joint_cfg)

                # Wheels use velocity, steering uses position
                if joint_cfg.joint_type in ("wheel", "continuous"):
                    await backend.send_velocity(actuator_name, hw_value)
                else:
                    is_servo = (
                        joint_cfg.actuator.backend
                        == ActuatorBackendType.KLIPPER_SERVO
                    )
                    await backend.send_position(
                        actuator_name, hw_value, is_servo=is_servo
                    )
                results[joint_name] = {"status": "ok", "hw_value": hw_value}
            except Exception as exc:
                errors.append(f"{joint_name}: {exc}")
                results[joint_name] = {"status": "error", "error": str(exc)}

        status = "ok" if not errors else "partial" if results else "error"
        response: dict[str, Any] = {
            "status": status,
            "robot_id": robot_id,
            "solver": profile.robot_type.value,
            "results": results,
        }
        if errors:
            response["errors"] = errors

        logger.info(
            "vehicle_driven",
            robot_id=robot_id,
            solver=profile.robot_type.value,
            linear_x=cmd.linear_x,
            angular_z=cmd.angular_z,
        )

        return response

    async def get_state(self, robot_id: str) -> RobotState:
        """
        Get the current state of a robot.

        Returns last-known joint positions from state tracking.
        Future: query backends for live positions.

        Args:
            robot_id: Robot profile ID.

        Returns:
            Current robot state.
        """
        from arbor_core.robotics.registry import RegistryError

        try:
            self._registry.get(robot_id)
        except RegistryError as exc:
            raise EngineError(str(exc)) from exc

        joint_positions = self._joint_states.get(robot_id, {})

        return RobotState(
            robot_id=robot_id,
            joint_positions=joint_positions,
            timestamp=time.time(),
        )

    async def home(self, robot_id: str) -> dict[str, Any]:
        """
        Home all joints to their configured home positions.

        Args:
            robot_id: Robot profile ID.

        Returns:
            Joint move results.
        """
        from arbor_core.robotics.registry import RegistryError

        try:
            profile = self._registry.get(robot_id)
        except RegistryError as exc:
            raise EngineError(str(exc)) from exc

        home_values = {j.name: j.home_value for j in profile.joints}
        logger.info("robot_homing", robot_id=robot_id, joints=list(home_values.keys()))
        return await self.move_joints(robot_id, home_values)

    async def stop(self, robot_id: str) -> dict[str, Any]:
        """
        Stop all motion on a robot immediately.

        Args:
            robot_id: Robot profile ID.

        Returns:
            Stop results per joint.
        """
        from arbor_core.robotics.registry import RegistryError

        try:
            profile = self._registry.get(robot_id)
        except RegistryError as exc:
            raise EngineError(str(exc)) from exc

        results: dict[str, Any] = {}
        errors: list[str] = []

        for joint_cfg in profile.joints:
            try:
                backend = self._get_backend(joint_cfg.actuator.backend)
                actuator_name = self._actuator_name(joint_cfg)
                await backend.stop(actuator_name)
                results[joint_cfg.name] = {"status": "stopped"}
            except Exception as exc:
                errors.append(f"{joint_cfg.name}: {exc}")
                results[joint_cfg.name] = {"status": "error", "error": str(exc)}

        logger.info("robot_stopped", robot_id=robot_id)

        return {
            "status": "ok" if not errors else "partial",
            "robot_id": robot_id,
            "results": results,
            **({"errors": errors} if errors else {}),
        }

    @staticmethod
    def _actuator_name(joint_cfg: Any) -> str:
        """
        Build the backend-specific actuator identifier for a joint.

        For arbor_servo: "node_id:servo_id"
        For klipper_stepper/klipper_servo: klipper_name
        """
        actuator = joint_cfg.actuator
        if actuator.backend == ActuatorBackendType.ARBOR_SERVO:
            return f"{actuator.node_id}:{actuator.servo_id}"
        else:
            return actuator.klipper_name or ""
