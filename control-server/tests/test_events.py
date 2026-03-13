"""
Tests for event bus and WebSocket infrastructure (C14).

Covers:
- Event creation with auto-timestamp
- EventBus subscribe/unsubscribe
- Topic pattern matching (glob wildcards)
- Event delivery to matching subscribers
- Queue overflow handling (drop, not block)
- Multiple subscribers with different patterns
- Subscriber count tracking
"""

from __future__ import annotations

import asyncio

import pytest

from arbor_core.events import Event, EventBus


class TestEvent:
    """Event model."""

    def test_auto_timestamp(self) -> None:
        e = Event(topic="test.event", data={"key": "value"})
        assert e.timestamp > 0

    def test_explicit_timestamp(self) -> None:
        e = Event(topic="test", timestamp=123.456)
        assert e.timestamp == 123.456

    def test_empty_data(self) -> None:
        e = Event(topic="test")
        assert e.data == {}


class TestEventBusSubscription:
    """Subscribe and unsubscribe."""

    async def test_subscribe_returns_queue(self) -> None:
        bus = EventBus()
        queue = await bus.subscribe(["test.*"])
        assert isinstance(queue, asyncio.Queue)
        assert bus.subscriber_count == 1

    async def test_unsubscribe_removes(self) -> None:
        bus = EventBus()
        queue = await bus.subscribe(["test.*"])
        await bus.unsubscribe(queue)
        assert bus.subscriber_count == 0

    async def test_unsubscribe_idempotent(self) -> None:
        bus = EventBus()
        queue = await bus.subscribe(["test.*"])
        await bus.unsubscribe(queue)
        await bus.unsubscribe(queue)  # No error
        assert bus.subscriber_count == 0

    async def test_multiple_subscribers(self) -> None:
        bus = EventBus()
        await bus.subscribe(["a.*"])
        await bus.subscribe(["b.*"])
        assert bus.subscriber_count == 2


class TestEventBusPublish:
    """Event delivery."""

    async def test_matching_event_delivered(self) -> None:
        bus = EventBus()
        queue = await bus.subscribe(["servo.*"])
        delivered = bus.publish(Event(topic="servo.position_changed", data={"id": 1}))
        assert delivered == 1
        event = queue.get_nowait()
        assert event.topic == "servo.position_changed"
        assert event.data == {"id": 1}

    async def test_non_matching_event_not_delivered(self) -> None:
        bus = EventBus()
        queue = await bus.subscribe(["servo.*"])
        delivered = bus.publish(Event(topic="sensor.temp.reading"))
        assert delivered == 0
        assert queue.empty()

    async def test_wildcard_all(self) -> None:
        bus = EventBus()
        queue = await bus.subscribe(["*"])
        bus.publish(Event(topic="anything"))
        assert not queue.empty()

    async def test_multiple_patterns(self) -> None:
        bus = EventBus()
        queue = await bus.subscribe(["servo.*", "sensor.*"])
        bus.publish(Event(topic="servo.move"))
        bus.publish(Event(topic="sensor.temp"))
        bus.publish(Event(topic="system.health"))  # No match
        assert queue.qsize() == 2

    async def test_multiple_subscribers_receive(self) -> None:
        bus = EventBus()
        q1 = await bus.subscribe(["servo.*"])
        q2 = await bus.subscribe(["servo.*", "sensor.*"])
        delivered = bus.publish(Event(topic="servo.move"))
        assert delivered == 2
        assert not q1.empty()
        assert not q2.empty()

    async def test_queue_overflow_drops(self) -> None:
        bus = EventBus(max_queue_size=2)
        queue = await bus.subscribe(["*"])
        bus.publish(Event(topic="a"))
        bus.publish(Event(topic="b"))
        bus.publish(Event(topic="c"))  # Should be dropped
        assert queue.qsize() == 2

    async def test_publish_returns_zero_no_subscribers(self) -> None:
        bus = EventBus()
        delivered = bus.publish(Event(topic="nobody.listening"))
        assert delivered == 0


class TestTopicPatterns:
    """Glob pattern matching edge cases."""

    async def test_exact_match(self) -> None:
        bus = EventBus()
        queue = await bus.subscribe(["system.health"])
        bus.publish(Event(topic="system.health"))
        assert not queue.empty()

    async def test_nested_wildcard(self) -> None:
        bus = EventBus()
        queue = await bus.subscribe(["sensor.*"])
        # fnmatch '.' is literal, '*' matches anything
        bus.publish(Event(topic="sensor.temp"))
        bus.publish(Event(topic="sensor.distance"))
        assert queue.qsize() == 2

    async def test_no_partial_match(self) -> None:
        bus = EventBus()
        queue = await bus.subscribe(["servo"])
        bus.publish(Event(topic="servo.move"))  # "servo" pattern doesn't match "servo.move"
        assert queue.empty()
