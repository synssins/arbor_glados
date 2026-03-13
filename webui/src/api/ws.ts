/**
 * WebSocket client with subscribe/publish protocol and auto-reconnect.
 * Task: W03
 */

type MessageHandler = (data: unknown) => void

interface WsEvent {
  type: 'event'
  topic: string
  data: unknown
}

interface WsResult {
  type: 'result'
  cmd: string
  data: unknown
  error?: string
}

interface WsError {
  type: 'error'
  detail: string
}

type WsMessage = WsEvent | WsResult | WsError

export class ArborWs {
  private ws: WebSocket | null = null
  private url: string
  private reconnectMs = 2000
  private maxReconnectMs = 30000
  private currentReconnectMs = 2000
  private subscriptions = new Map<string, Set<MessageHandler>>()
  private pendingSubscribes: string[] = []
  private _connected = false
  private _onConnectionChange: ((connected: boolean) => void) | null = null
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null

  constructor(url?: string) {
    const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
    this.url = url || `${proto}//${window.location.host}/api/v1/ws`
  }

  get connected() {
    return this._connected
  }

  onConnectionChange(handler: (connected: boolean) => void) {
    this._onConnectionChange = handler
  }

  connect() {
    if (this.ws?.readyState === WebSocket.OPEN) return

    const token = localStorage.getItem('sb_token')
    const url = token ? `${this.url}?token=${token}` : this.url

    this.ws = new WebSocket(url)

    this.ws.onopen = () => {
      this._connected = true
      this.currentReconnectMs = this.reconnectMs
      this._onConnectionChange?.(true)

      // Re-subscribe to all topics
      for (const topic of this.subscriptions.keys()) {
        this.sendSubscribe(topic)
      }
      // Send any pending subscribes
      for (const topic of this.pendingSubscribes) {
        this.sendSubscribe(topic)
      }
      this.pendingSubscribes = []
    }

    this.ws.onclose = () => {
      this._connected = false
      this._onConnectionChange?.(false)
      this.scheduleReconnect()
    }

    this.ws.onerror = () => {
      this.ws?.close()
    }

    this.ws.onmessage = (event) => {
      try {
        const msg: WsMessage = JSON.parse(event.data)
        this.handleMessage(msg)
      } catch {
        // Ignore malformed messages
      }
    }
  }

  disconnect() {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer)
      this.reconnectTimer = null
    }
    this.ws?.close()
    this.ws = null
    this._connected = false
  }

  subscribe(topic: string, handler: MessageHandler) {
    if (!this.subscriptions.has(topic)) {
      this.subscriptions.set(topic, new Set())
      if (this._connected) {
        this.sendSubscribe(topic)
      } else {
        this.pendingSubscribes.push(topic)
      }
    }
    this.subscriptions.get(topic)!.add(handler)

    // Return unsubscribe function
    return () => {
      const handlers = this.subscriptions.get(topic)
      if (handlers) {
        handlers.delete(handler)
        if (handlers.size === 0) {
          this.subscriptions.delete(topic)
          this.sendUnsubscribe(topic)
        }
      }
    }
  }

  sendCommand(plugin: string, cmd: string, params?: unknown) {
    this.send({
      type: 'command',
      plugin,
      cmd,
      params: params || {},
    })
  }

  private sendSubscribe(topic: string) {
    this.send({ type: 'subscribe', topic })
  }

  private sendUnsubscribe(topic: string) {
    this.send({ type: 'unsubscribe', topic })
  }

  private send(data: unknown) {
    if (this.ws?.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(data))
    }
  }

  private handleMessage(msg: WsMessage) {
    if (msg.type === 'event') {
      const event = msg as WsEvent
      // Deliver to matching subscribers
      for (const [pattern, handlers] of this.subscriptions) {
        if (this.topicMatches(pattern, event.topic)) {
          for (const handler of handlers) {
            handler(event.data)
          }
        }
      }
    }
  }

  private topicMatches(pattern: string, topic: string): boolean {
    if (pattern === '*') return true
    if (!pattern.includes('*')) return pattern === topic
    const prefix = pattern.slice(0, pattern.indexOf('*'))
    return topic.startsWith(prefix)
  }

  private scheduleReconnect() {
    if (this.reconnectTimer) return
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null
      this.connect()
      // Exponential backoff
      this.currentReconnectMs = Math.min(
        this.currentReconnectMs * 1.5,
        this.maxReconnectMs,
      )
    }, this.currentReconnectMs)
  }
}

// Singleton instance
export const ws = new ArborWs()
