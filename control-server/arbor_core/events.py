"""
Event bus — in-process publish/subscribe system.

Central event bus that plugins, bridges, and the WebSocket server use
to communicate. Supports topic-based subscriptions with glob patterns.

Task: C14
"""

from __future__ import annotations

import asyncio
import fnmatch
import time
from typing import Any

import structlog
from pydantic import BaseModel

logger = structlog.get_logger(__name__)


class Event(BaseModel):
    """An event published on the event bus."""

    topic: str
    data: dict[str, Any] = {}
    timestamp: float = 0.0

    def __init__(self, **kwargs: Any) -> None:
        super().__init__(**kwargs)
        if self.timestamp == 0.0:
            self.timestamp = time.time()


class EventBus:
    """
    In-process async event bus with topic-based pub/sub.

    Subscribers register with topic patterns (supports glob wildcards).
    Events are delivered to matching subscribers via asyncio.Queue.

    Usage::

        bus = EventBus()
        queue = bus.subscribe(["servo.*", "sensor.*"])
        bus.publish(Event(topic="servo.position_changed", data={"id": 1, "position": 512}))
        event = await queue.get()
        bus.unsubscribe(queue)
    """

    def __init__(self, max_queue_size: int = 256) -> None:
        """
        Args:
            max_queue_size: Maximum events buffered per subscriber queue.
        """
        self._max_queue_size = max_queue_size
        self._subscribers: dict[asyncio.Queue[Event], list[str]] = {}
        self._lock = asyncio.Lock()

    async def subscribe(self, patterns: list[str]) -> asyncio.Queue[Event]:
        """
        Subscribe to events matching the given topic patterns.

        Patterns support glob wildcards:
        - "servo.*" matches "servo.position_changed", "servo.torque_changed"
        - "sensor.*" matches "sensor.temp.reading"
        - "*" matches everything

        Args:
            patterns: List of topic glob patterns.

        Returns:
            asyncio.Queue that will receive matching events.
        """
        queue: asyncio.Queue[Event] = asyncio.Queue(maxsize=self._max_queue_size)
        async with self._lock:
            self._subscribers[queue] = patterns
        logger.debug("event_bus_subscribe", patterns=patterns)
        return queue

    async def unsubscribe(self, queue: asyncio.Queue[Event]) -> None:
        """
        Remove a subscriber.

        Args:
            queue: The queue returned by subscribe().
        """
        async with self._lock:
            self._subscribers.pop(queue, None)
        logger.debug("event_bus_unsubscribe")

    def publish(self, event: Event) -> int:
        """
        Publish an event to all matching subscribers.

        Non-blocking. If a subscriber's queue is full, the event is
        dropped for that subscriber (logged as warning).

        Args:
            event: The event to publish.

        Returns:
            Number of subscribers that received the event.
        """
        delivered = 0
        for queue, patterns in self._subscribers.items():
            if self._matches(event.topic, patterns):
                try:
                    queue.put_nowait(event)
                    delivered += 1
                except asyncio.QueueFull:
                    logger.warning(
                        "event_bus_queue_full",
                        topic=event.topic,
                        dropped=True,
                    )
        return delivered

    @property
    def subscriber_count(self) -> int:
        """Number of active subscribers."""
        return len(self._subscribers)

    @staticmethod
    def _matches(topic: str, patterns: list[str]) -> bool:
        """Check if a topic matches any of the patterns."""
        return any(fnmatch.fnmatch(topic, p) for p in patterns)
