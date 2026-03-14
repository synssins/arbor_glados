/**
 * Robot control view — profile management, joint sliders, drive pad, state display.
 * Task: Robotics Phase 4
 */

import { useEffect, useState } from 'react'
import { useRobotStore } from '../stores/robot'
import type { RobotProfile, JointMoveCommand, VelocityCommand, CartesianPose } from '../api/client'

/* ── Helpers ── */

const ROBOT_TYPE_LABELS: Record<string, string> = {
  serial_arm: '🦾 Serial Arm',
  scara: '🦾 SCARA',
  differential: '🏎️ Differential',
  ackermann: '🚗 Ackermann',
  mecanum: '🛞 Mecanum',
}

function isVehicle(type: string): boolean {
  return ['differential', 'ackermann', 'mecanum'].includes(type)
}

function isArm(type: string): boolean {
  return ['serial_arm', 'scara'].includes(type)
}

/* ── Components ── */

function JointSliders({ profile }: { profile: RobotProfile }) {
  const moveJoints = useRobotStore((s) => s.moveJoints)
  const states = useRobotStore((s) => s.states)
  const state = states.get(profile.id)
  const [speed, setSpeed] = useState<number>(100)

  const handleMove = (jointName: string, value: number) => {
    const cmd: JointMoveCommand = { positions: { [jointName]: value }, speed }
    moveJoints(profile.id, cmd)
  }

  return (
    <div className="space-y-3">
      <h3 className="font-semibold text-sm text-gray-600 uppercase">Joint Control</h3>
      <div className="flex items-center gap-2 text-sm">
        <label className="text-gray-500">Speed:</label>
        <input
          type="range" min={0} max={2000} value={speed}
          onChange={(e) => setSpeed(Number(e.target.value))}
          className="flex-1"
        />
        <span className="w-16 text-right">{speed}</span>
      </div>
      {profile.joints.map((joint) => {
        const currentVal = state?.joint_positions[joint.name] ?? joint.home_value
        const min = joint.min_value ?? -3.14159
        const max = joint.max_value ?? 3.14159
        return (
          <div key={joint.name} className="flex items-center gap-2">
            <span className="w-28 text-sm font-medium truncate" title={joint.name}>
              {joint.name}
            </span>
            <input
              type="range"
              min={min} max={max} step={0.01}
              value={currentVal}
              onChange={(e) => handleMove(joint.name, Number(e.target.value))}
              className="flex-1"
            />
            <span className="w-20 text-xs text-right font-mono">
              {currentVal.toFixed(3)}
            </span>
            <span className="w-16 text-xs text-gray-400">
              {joint.actuator.backend === 'arbor_servo' ? 'Arbor' : 'Klipper'}
            </span>
          </div>
        )
      })}
    </div>
  )
}

function DrivePad({ profile }: { profile: RobotProfile }) {
  const drive = useRobotStore((s) => s.drive)
  const [linearX, setLinearX] = useState(0)
  const [linearY, setLinearY] = useState(0)
  const [angularZ, setAngularZ] = useState(0)

  const sendDrive = () => {
    const cmd: VelocityCommand = {
      linear_x: linearX,
      linear_y: linearY,
      angular_z: angularZ,
    }
    drive(profile.id, cmd)
  }

  const stopAll = () => {
    setLinearX(0)
    setLinearY(0)
    setAngularZ(0)
    drive(profile.id, { linear_x: 0, linear_y: 0, angular_z: 0 })
  }

  const isMecanum = profile.robot_type === 'mecanum'

  return (
    <div className="space-y-3">
      <h3 className="font-semibold text-sm text-gray-600 uppercase">Drive Control</h3>
      <div className="space-y-2">
        <div className="flex items-center gap-2">
          <label className="w-24 text-sm">Forward:</label>
          <input type="range" min={-2} max={2} step={0.1} value={linearX}
            onChange={(e) => setLinearX(Number(e.target.value))} className="flex-1" />
          <span className="w-16 text-xs text-right font-mono">{linearX.toFixed(1)} m/s</span>
        </div>
        {isMecanum && (
          <div className="flex items-center gap-2">
            <label className="w-24 text-sm">Strafe:</label>
            <input type="range" min={-2} max={2} step={0.1} value={linearY}
              onChange={(e) => setLinearY(Number(e.target.value))} className="flex-1" />
            <span className="w-16 text-xs text-right font-mono">{linearY.toFixed(1)} m/s</span>
          </div>
        )}
        <div className="flex items-center gap-2">
          <label className="w-24 text-sm">Turn:</label>
          <input type="range" min={-3} max={3} step={0.1} value={angularZ}
            onChange={(e) => setAngularZ(Number(e.target.value))} className="flex-1" />
          <span className="w-16 text-xs text-right font-mono">{angularZ.toFixed(1)} rad/s</span>
        </div>
      </div>
      <div className="flex gap-2">
        <button onClick={sendDrive}
          className="px-4 py-2 bg-blue-600 text-white rounded hover:bg-blue-700 text-sm">
          Send
        </button>
        <button onClick={stopAll}
          className="px-4 py-2 bg-red-600 text-white rounded hover:bg-red-700 text-sm">
          STOP
        </button>
      </div>
    </div>
  )
}

function CartesianControl({ profile }: { profile: RobotProfile }) {
  const moveCartesian = useRobotStore((s) => s.moveCartesian)
  const [pose, setPose] = useState<CartesianPose>({
    x: 0, y: 0, z: 0.2, roll: 0, pitch: 0, yaw: 0,
  })

  const handleMove = () => {
    moveCartesian(profile.id, pose)
  }

  const fields: { key: keyof CartesianPose; label: string; unit: string; step: number }[] = [
    { key: 'x', label: 'X', unit: 'm', step: 0.01 },
    { key: 'y', label: 'Y', unit: 'm', step: 0.01 },
    { key: 'z', label: 'Z', unit: 'm', step: 0.01 },
    { key: 'roll', label: 'Roll', unit: 'rad', step: 0.1 },
    { key: 'pitch', label: 'Pitch', unit: 'rad', step: 0.1 },
    { key: 'yaw', label: 'Yaw', unit: 'rad', step: 0.1 },
  ]

  return (
    <div className="space-y-3">
      <h3 className="font-semibold text-sm text-gray-600 uppercase">Cartesian Move (IK)</h3>
      <div className="grid grid-cols-3 gap-2">
        {fields.map((f) => (
          <div key={f.key} className="flex items-center gap-1">
            <label className="text-xs w-10">{f.label}:</label>
            <input type="number" step={f.step}
              value={pose[f.key]}
              onChange={(e) => setPose({ ...pose, [f.key]: Number(e.target.value) })}
              className="w-full text-xs px-1 py-0.5 border rounded font-mono"
            />
            <span className="text-xs text-gray-400">{f.unit}</span>
          </div>
        ))}
      </div>
      <button onClick={handleMove}
        className="px-4 py-2 bg-blue-600 text-white rounded hover:bg-blue-700 text-sm">
        Move to Pose
      </button>
    </div>
  )
}

function StateDisplay({ profile }: { profile: RobotProfile }) {
  const states = useRobotStore((s) => s.states)
  const fetchState = useRobotStore((s) => s.fetchState)
  const state = states.get(profile.id)

  return (
    <div className="space-y-2">
      <div className="flex items-center justify-between">
        <h3 className="font-semibold text-sm text-gray-600 uppercase">State</h3>
        <button onClick={() => fetchState(profile.id)}
          className="text-xs text-blue-600 hover:underline">Refresh</button>
      </div>
      {state ? (
        <div className="text-xs font-mono space-y-1">
          {Object.entries(state.joint_positions).map(([name, val]) => (
            <div key={name} className="flex justify-between">
              <span>{name}</span>
              <span>{(val as number).toFixed(4)}</span>
            </div>
          ))}
          {state.end_effector_pose && (
            <div className="mt-2 pt-2 border-t">
              <div className="text-gray-500 mb-1">End Effector:</div>
              <div>x={state.end_effector_pose.x.toFixed(3)} y={state.end_effector_pose.y.toFixed(3)} z={state.end_effector_pose.z.toFixed(3)}</div>
            </div>
          )}
        </div>
      ) : (
        <div className="text-xs text-gray-400">No state data</div>
      )}
    </div>
  )
}

function ResultDisplay() {
  const lastResult = useRobotStore((s) => s.lastResult)
  const error = useRobotStore((s) => s.error)

  if (error) {
    return <div className="text-xs text-red-600 bg-red-50 p-2 rounded">Error: {error}</div>
  }
  if (!lastResult) return null

  return (
    <div className={`text-xs p-2 rounded ${
      lastResult.status === 'ok' ? 'bg-green-50 text-green-700' :
      lastResult.status === 'partial' ? 'bg-yellow-50 text-yellow-700' :
      'bg-red-50 text-red-700'
    }`}>
      <span className="font-semibold">{lastResult.status}</span>
      {lastResult.message && <span className="ml-2">{lastResult.message}</span>}
      {lastResult.errors && lastResult.errors.length > 0 && (
        <ul className="mt-1 list-disc list-inside">
          {lastResult.errors.map((e, i) => <li key={i}>{e}</li>)}
        </ul>
      )}
    </div>
  )
}

/* ── Main View ── */

export default function RobotControl() {
  const profiles = useRobotStore((s) => s.profiles)
  const selectedId = useRobotStore((s) => s.selectedId)
  const loading = useRobotStore((s) => s.loading)
  const fetchProfiles = useRobotStore((s) => s.fetchProfiles)
  const selectRobot = useRobotStore((s) => s.selectRobot)
  const homeRobot = useRobotStore((s) => s.homeRobot)
  const stopRobot = useRobotStore((s) => s.stopRobot)

  useEffect(() => {
    fetchProfiles()
  }, [fetchProfiles])

  const selected = profiles.find((p) => p.id === selectedId) ?? null

  return (
    <div className="space-y-6">
      <h2 className="text-xl font-bold">Robot Control</h2>

      {/* Robot selector */}
      <div className="flex items-center gap-4">
        <select
          value={selectedId ?? ''}
          onChange={(e) => selectRobot(e.target.value || null)}
          className="border rounded px-3 py-2 text-sm"
        >
          <option value="">Select a robot...</option>
          {profiles.map((p) => (
            <option key={p.id} value={p.id}>
              {p.name} ({ROBOT_TYPE_LABELS[p.robot_type] ?? p.robot_type})
            </option>
          ))}
        </select>
        {loading && <span className="text-sm text-gray-500">Loading...</span>}
        {profiles.length === 0 && !loading && (
          <span className="text-sm text-gray-400">
            No robots configured. Create one via the API.
          </span>
        )}
      </div>

      {/* Selected robot controls */}
      {selected && (
        <div className="grid grid-cols-1 lg:grid-cols-2 gap-6">
          {/* Left column: Profile info + Controls */}
          <div className="space-y-4">
            {/* Profile info card */}
            <div className="card p-4">
              <div className="flex items-center justify-between mb-2">
                <h3 className="font-bold">{selected.name}</h3>
                <span className="text-sm bg-gray-100 px-2 py-1 rounded">
                  {ROBOT_TYPE_LABELS[selected.robot_type] ?? selected.robot_type}
                </span>
              </div>
              {selected.description && (
                <p className="text-sm text-gray-500 mb-2">{selected.description}</p>
              )}
              <div className="text-xs text-gray-400">
                {selected.joints.length} joints • ID: {selected.id}
              </div>
              <div className="flex gap-2 mt-3">
                <button onClick={() => homeRobot(selected.id)}
                  className="px-3 py-1 bg-gray-200 hover:bg-gray-300 rounded text-sm">
                  Home
                </button>
                <button onClick={() => stopRobot(selected.id)}
                  className="px-3 py-1 bg-red-600 text-white hover:bg-red-700 rounded text-sm">
                  STOP
                </button>
              </div>
            </div>

            {/* Motion controls */}
            <div className="card p-4">
              <JointSliders profile={selected} />
            </div>

            {isVehicle(selected.robot_type) && (
              <div className="card p-4">
                <DrivePad profile={selected} />
              </div>
            )}

            {isArm(selected.robot_type) && (
              <div className="card p-4">
                <CartesianControl profile={selected} />
              </div>
            )}
          </div>

          {/* Right column: State + Results */}
          <div className="space-y-4">
            <div className="card p-4">
              <StateDisplay profile={selected} />
            </div>
            <div className="card p-4">
              <ResultDisplay />
            </div>
          </div>
        </div>
      )}
    </div>
  )
}
