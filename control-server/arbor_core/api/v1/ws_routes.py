"""
WebSocket endpoint — subscribe/publish real-time events.

Per ARBOR_PROJECT_PLAN.md Section 6:
    WS /api/v1/ws — WebSocket subscribe/publish

Protocol:
    Client -> Server:
        {"action": "subscribe", "topics": ["servo.*", "sensor.*"]}
        {"action": "command", "module": "servo", "id": "1", "cmd": "set_position", "params": {"position": 512}}

    Server -> Client:
        {"event": "servo.position_changed", "id": "1", "data": {"position": 512, "timestamp": "..."}}

Authentication: Bearer token sent as query param `token` or first message.

Task: C14
"""

from __future__ import annotations

import asyncio
import json
from typing import Any

import structlog
from fastapi import APIRouter, Query, WebSocket, WebSocketDisconnect

from arbor_core.auth.api_keys import APIKeyManager
from arbor_core.auth.jwt import JWTError, JWTManager
from arbor_core.events import Event, EventBus

logger = structlog.get_logger(__name__)

ws_router = APIRouter(tags=["websocket"])


@ws_router.websocket("/ws")
async def websocket_endpoint(
    websocket: WebSocket,
    token: str | None = Query(default=None),
) -> None:
    """
    WebSocket endpoint for real-time event streaming.

    Authentication via query param `token` (Bearer token value).
    """
    # Authenticate before accepting
    api_key_mgr: APIKeyManager | None = getattr(
        websocket.app.state, "api_key_manager", None
    )
    jwt_mgr: JWTManager | None = getattr(websocket.app.state, "jwt_manager", None)
    event_bus: EventBus | None = getattr(websocket.app.state, "event_bus", None)

    if not token or not _authenticate(token, api_key_mgr, jwt_mgr):
        await websocket.close(code=4001, reason="Authentication required")
        return

    if event_bus is None:
        await websocket.close(code=4002, reason="Event bus not available")
        return

    await websocket.accept()
    logger.info("ws_connected", client=str(websocket.client))

    # Default subscription — empty until client subscribes
    queue: asyncio.Queue[Event] | None = None

    try:
        # Run send and receive concurrently
        send_task = asyncio.create_task(_send_events(websocket, event_bus, queue))
        recv_task = asyncio.create_task(
            _receive_messages(websocket, event_bus, send_task)
        )

        done, pending = await asyncio.wait(
            [send_task, recv_task],
            return_when=asyncio.FIRST_COMPLETED,
        )

        for task in pending:
            task.cancel()
            try:
                await task
            except (asyncio.CancelledError, WebSocketDisconnect):
                pass

    except WebSocketDisconnect:
        logger.info("ws_disconnected", client=str(websocket.client))
    except Exception:
        logger.exception("ws_error", client=str(websocket.client))
    finally:
        # Cleanup handled inside tasks
        pass


async def _send_events(
    websocket: WebSocket,
    event_bus: EventBus,
    queue: asyncio.Queue[Event] | None,
) -> None:
    """Send events from the queue to the WebSocket client."""
    # Wait until we have a subscription queue
    while queue is None:
        await asyncio.sleep(0.1)
        # Check if the websocket is still alive
        try:
            queue = getattr(websocket.state, "_event_queue", None)
        except Exception:
            return


    try:
        while True:
            event = await queue.get()
            await websocket.send_json({
                "event": event.topic,
                "data": event.data,
                "timestamp": event.timestamp,
            })
    except (WebSocketDisconnect, asyncio.CancelledError):
        pass
    finally:
        await event_bus.unsubscribe(queue)


async def _receive_messages(
    websocket: WebSocket,
    event_bus: EventBus,
    send_task: asyncio.Task[None],
) -> None:
    """Receive and process messages from the WebSocket client."""
    try:
        while True:
            raw = await websocket.receive_text()
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                await websocket.send_json({"error": "Invalid JSON"})
                continue

            action = msg.get("action")

            if action == "subscribe":
                topics = msg.get("topics", [])
                if not isinstance(topics, list) or not topics:
                    await websocket.send_json(
                        {"error": "topics must be a non-empty list"}
                    )
                    continue

                # Create subscription
                queue = await event_bus.subscribe(topics)
                websocket.state._event_queue = queue  # type: ignore[attr-defined]

                # Restart send task with the new queue
                send_task.cancel()

                await websocket.send_json({
                    "action": "subscribed",
                    "topics": topics,
                })
                logger.info("ws_subscribed", topics=topics)

                # Start new send loop
                new_send = asyncio.create_task(
                    _send_events(websocket, event_bus, queue)
                )
                # Store for cleanup
                websocket.state._send_task = new_send  # type: ignore[attr-defined]

            elif action == "command":
                # Commands dispatched to plugin manager (future: C11/C12)
                module = msg.get("module", "")
                cmd = msg.get("cmd", "")
                params = msg.get("params", {})

                plugin_mgr = getattr(websocket.app.state, "plugin_manager", None)
                if plugin_mgr is None:
                    await websocket.send_json({"error": "No plugin manager"})
                    continue

                instance = plugin_mgr.get_instance(module)
                if instance is None:
                    await websocket.send_json(
                        {"error": f"Module '{module}' not loaded"}
                    )
                    continue

                try:
                    result = await instance.handle_command(cmd, params)
                    await websocket.send_json({
                        "action": "command_result",
                        "module": module,
                        "cmd": cmd,
                        "result": result,
                    })
                except Exception as exc:
                    await websocket.send_json({
                        "error": f"Command failed: {exc}",
                    })

            elif action == "ping":
                await websocket.send_json({"action": "pong"})

            else:
                await websocket.send_json(
                    {"error": f"Unknown action: {action}"}
                )

    except (WebSocketDisconnect, asyncio.CancelledError):
        pass


def _authenticate(
    token: str,
    api_key_mgr: APIKeyManager | None,
    jwt_mgr: JWTManager | None,
) -> bool:
    """Verify a Bearer token (API key or JWT)."""
    if api_key_mgr is not None:
        record = api_key_mgr.verify_key(token)
        if record is not None:
            # Check for stream scope
            scope_values = [s.value for s in record.scopes]
            if "stream" in scope_values or "admin" in scope_values:
                return True
            return False

    if jwt_mgr is not None:
        try:
            payload = jwt_mgr.verify(token)
            scopes = payload.get("scopes", [])
            return "stream" in scopes or "admin" in scopes
        except JWTError:
            pass

    return False
