/**
 * Nodes management page — list registered nodes, add new nodes with auto-detection.
 *
 * Three-step inline wizard:
 *   1. Enter connection info (network IP or serial port)
 *   2. View detection results (probe response)
 *   3. Confirm and register the node
 */

import { useEffect, useState } from 'react'
import { useNodeStore } from '../stores/nodes'
import type { ProbeResult, NodeAddRequest } from '../api/client'

type TransportMode = 'network' | 'serial'
type WizardStep = 'idle' | 'connect' | 'detect' | 'confirm' | 'done'

export default function Nodes() {
  const { nodes, loading, error, probing, probeResult, probeError, fetchNodes, probeNode, addNode, removeNode, clearProbe, clearError } = useNodeStore()
  const [step, setStep] = useState<WizardStep>('idle')

  useEffect(() => {
    fetchNodes()
  }, [fetchNodes])

  return (
    <div className="space-y-6">
      <div className="flex items-center justify-between">
        <h2 className="text-xl font-bold text-gray-800">Nodes</h2>
        {step === 'idle' && (
          <button
            onClick={() => { clearProbe(); clearError(); setStep('connect') }}
            className="btn-primary text-sm"
          >
            + Add Node
          </button>
        )}
        {step !== 'idle' && step !== 'done' && (
          <button
            onClick={() => { setStep('idle'); clearProbe() }}
            className="btn-secondary text-sm"
          >
            Cancel
          </button>
        )}
      </div>

      {/* Add Node Wizard */}
      {step !== 'idle' && (
        <AddNodeWizard
          step={step}
          setStep={setStep}
          probing={probing}
          probeResult={probeResult}
          probeError={probeError}
          onProbe={probeNode}
          onAdd={addNode}
          onDone={() => { setStep('idle'); clearProbe(); fetchNodes() }}
        />
      )}

      {/* Error display */}
      {error && (
        <div className="card bg-red-50 border border-red-200 text-red-700 text-sm">
          {error}
        </div>
      )}

      {/* Node List */}
      <div className="card">
        <h3 className="text-sm font-semibold text-gray-700 mb-3">Registered Nodes</h3>
        {loading ? (
          <p className="text-sm text-gray-400">Loading...</p>
        ) : nodes.length === 0 ? (
          <p className="text-sm text-gray-500">
            No nodes registered yet. Click <span className="font-medium">"+ Add Node"</span> to connect your first device.
          </p>
        ) : (
          <div className="space-y-2">
            {nodes.map((node) => (
              <div key={node.id} className="flex items-center justify-between py-3 px-3 border border-gray-100 rounded-lg hover:bg-gray-50 transition-colors">
                <div className="flex items-center gap-3">
                  <div className={`w-2.5 h-2.5 rounded-full flex-shrink-0 ${
                    node.status === 'connected' ? 'bg-green-500' :
                    node.status === 'disconnected' ? 'bg-red-500' : 'bg-gray-400'
                  }`} />
                  <div>
                    <span className="text-sm font-medium text-gray-800">{node.id}</span>
                    <span className="text-xs text-gray-400 ml-2">
                      {node.type} via {node.transport}
                    </span>
                    {node.host && (
                      <span className="text-xs text-gray-400 ml-2">{node.host}{node.port ? `:${node.port}` : ''}</span>
                    )}
                    {node.serial_port && (
                      <span className="text-xs text-gray-400 ml-2">{node.serial_port}</span>
                    )}
                  </div>
                </div>
                <button
                  onClick={() => { if (confirm(`Remove node "${node.id}"?`)) removeNode(node.id) }}
                  className="text-xs text-red-500 hover:text-red-700 px-2 py-1 rounded hover:bg-red-50 transition-colors"
                >
                  Remove
                </button>
              </div>
            ))}
          </div>
        )}
      </div>
    </div>
  )
}

// ── Add Node Wizard ──

interface WizardProps {
  step: WizardStep
  setStep: (s: WizardStep) => void
  probing: boolean
  probeResult: ProbeResult | null
  probeError: string | null
  onProbe: (host: string, port: number) => Promise<void>
  onAdd: (node: NodeAddRequest) => Promise<boolean>
  onDone: () => void
}

function AddNodeWizard({ step, setStep, probing, probeResult, probeError, onProbe, onAdd, onDone }: WizardProps) {
  const [transport, setTransport] = useState<TransportMode>('network')
  const [host, setHost] = useState('')
  const [port, setPort] = useState('80')
  const [serialPort, setSerialPort] = useState('/dev/ttyUSB0')
  const [baud, setBaud] = useState('115200')
  const [nodeId, setNodeId] = useState('')
  const [adding, setAdding] = useState(false)

  // Auto-generate node ID from probe result
  useEffect(() => {
    if (probeResult?.info) {
      const info = probeResult.info
      const boardId = info.board_id || ''
      const platform = info.platform || 'node'
      const suffix = boardId ? boardId.slice(-8).toLowerCase() : host.replace(/\./g, '-')
      setNodeId(`${platform}-${suffix}`)
    }
  }, [probeResult, host])

  const handleDetect = async () => {
    if (transport === 'network') {
      await onProbe(host, parseInt(port) || 80)
    }
    setStep('detect')
  }

  const handleAdd = async () => {
    setAdding(true)
    const req: NodeAddRequest = transport === 'network'
      ? {
          id: nodeId,
          type: probeResult?.info?.platform || 'esp32',
          transport: 'wifi',
          transport_config: { host, network_port: parseInt(port) || 80 },
        }
      : {
          id: nodeId || `serial-${serialPort.replace(/\//g, '-').replace(/^-/, '')}`,
          type: 'esp32',
          transport: 'uart',
          transport_config: { port: serialPort, baud: parseInt(baud) || 115200 },
        }

    const ok = await onAdd(req)
    setAdding(false)
    if (ok) setStep('done')
  }

  return (
    <div className="card border-servo-200 border-2">
      {/* Step indicators */}
      <div className="flex items-center gap-2 mb-4">
        <StepDot active={step === 'connect'} done={step === 'detect' || step === 'confirm' || step === 'done'} label="1" />
        <div className="w-8 h-px bg-gray-300" />
        <StepDot active={step === 'detect'} done={step === 'confirm' || step === 'done'} label="2" />
        <div className="w-8 h-px bg-gray-300" />
        <StepDot active={step === 'confirm' || step === 'done'} done={step === 'done'} label="3" />
      </div>

      {/* Step 1: Connection */}
      {step === 'connect' && (
        <div className="space-y-4">
          <h3 className="text-sm font-semibold text-gray-700">Step 1: Connection Details</h3>

          {/* Transport selector */}
          <div className="flex gap-2">
            <button
              onClick={() => setTransport('network')}
              className={`px-4 py-2 rounded-lg text-sm font-medium transition-colors ${
                transport === 'network'
                  ? 'bg-servo-100 text-servo-700 ring-2 ring-servo-300'
                  : 'bg-gray-100 text-gray-600 hover:bg-gray-200'
              }`}
            >
              Network (WiFi / Ethernet)
            </button>
            <button
              onClick={() => setTransport('serial')}
              className={`px-4 py-2 rounded-lg text-sm font-medium transition-colors ${
                transport === 'serial'
                  ? 'bg-servo-100 text-servo-700 ring-2 ring-servo-300'
                  : 'bg-gray-100 text-gray-600 hover:bg-gray-200'
              }`}
            >
              Serial (UART / USB)
            </button>
          </div>

          {transport === 'network' ? (
            <div className="grid grid-cols-3 gap-3">
              <div className="col-span-2">
                <label className="block text-xs font-medium text-gray-500 mb-1">IP Address</label>
                <input
                  type="text"
                  className="input w-full"
                  placeholder="192.168.1.100"
                  value={host}
                  onChange={(e) => setHost(e.target.value)}
                />
              </div>
              <div>
                <label className="block text-xs font-medium text-gray-500 mb-1">Port</label>
                <input
                  type="text"
                  className="input w-full"
                  placeholder="80"
                  value={port}
                  onChange={(e) => setPort(e.target.value)}
                />
              </div>
            </div>
          ) : (
            <div className="grid grid-cols-3 gap-3">
              <div className="col-span-2">
                <label className="block text-xs font-medium text-gray-500 mb-1">Serial Port</label>
                <input
                  type="text"
                  className="input w-full"
                  placeholder="/dev/ttyUSB0"
                  value={serialPort}
                  onChange={(e) => setSerialPort(e.target.value)}
                />
              </div>
              <div>
                <label className="block text-xs font-medium text-gray-500 mb-1">Baud Rate</label>
                <select
                  className="input w-full"
                  value={baud}
                  onChange={(e) => setBaud(e.target.value)}
                >
                  <option value="9600">9600</option>
                  <option value="115200">115200</option>
                  <option value="921600">921600</option>
                  <option value="1000000">1000000</option>
                </select>
              </div>
            </div>
          )}

          <button
            onClick={handleDetect}
            disabled={transport === 'network' && !host}
            className="btn-primary text-sm disabled:opacity-50"
          >
            {transport === 'network' ? 'Detect' : 'Next'}
          </button>
        </div>
      )}

      {/* Step 2: Detection Results */}
      {step === 'detect' && (
        <div className="space-y-4">
          <h3 className="text-sm font-semibold text-gray-700">Step 2: Detection Results</h3>

          {probing && (
            <div className="flex items-center gap-2 text-sm text-gray-500">
              <div className="w-4 h-4 border-2 border-servo-500 border-t-transparent rounded-full animate-spin" />
              Probing {host}:{port}...
            </div>
          )}

          {probeError && (
            <div className="text-sm text-red-600 bg-red-50 rounded-lg p-3">
              Connection failed: {probeError}
            </div>
          )}

          {transport === 'serial' && !probing && (
            <div className="text-sm text-gray-500 bg-gray-50 rounded-lg p-3">
              Serial detection requires the node to be physically connected.
              Auto-detection is limited to connectivity checks for serial nodes.
            </div>
          )}

          {probeResult && (
            <div className="space-y-2">
              <ProbeStep label="Health check" ok={probeResult.reachable} error={probeResult.health_error} />
              <ProbeStep
                label="System info"
                ok={!!probeResult.info}
                error={probeResult.info_error}
                detail={probeResult.info ? `${probeResult.info.platform || 'unknown'} — ${probeResult.info.firmware_version || probeResult.info.arbor_version || 'unknown'}` : undefined}
              />
              <ProbeStep
                label="Configuration"
                ok={!!probeResult.config}
                error={probeResult.config_error}
              />
              <ProbeStep
                label="Servo scan"
                ok={!!probeResult.servo_scan}
                error={probeResult.servo_scan_error}
                detail={probeResult.servo_scan ? `Found ${probeResult.servo_scan.count} servo(s): IDs ${probeResult.servo_scan.found_ids.join(', ')}` : undefined}
              />

              {/* Plugin list */}
              {(probeResult.info?.plugins || probeResult.info?.loaded_plugins) && (
                <div className="mt-3 p-3 bg-gray-50 rounded-lg">
                  <span className="text-xs font-medium text-gray-500">Detected Modules:</span>
                  <div className="flex flex-wrap gap-1.5 mt-1.5">
                    {(probeResult.info?.plugins || probeResult.info?.loaded_plugins || []).map((p, i) => (
                      <span key={i} className="text-xs bg-servo-100 text-servo-700 px-2 py-0.5 rounded-full">
                        {p.name}
                      </span>
                    ))}
                  </div>
                </div>
              )}
            </div>
          )}

          <div className="flex gap-2 pt-2">
            {probeResult?.reachable && (
              <button onClick={() => setStep('confirm')} className="btn-primary text-sm">
                Looks Good
              </button>
            )}
            {transport === 'serial' && !probeResult && (
              <button onClick={() => setStep('confirm')} className="btn-primary text-sm">
                Continue Without Detection
              </button>
            )}
            <button onClick={() => { setStep('connect'); }} className="btn-secondary text-sm">
              {probeResult?.reachable ? 'Try Again' : 'Back'}
            </button>
          </div>
        </div>
      )}

      {/* Step 3: Confirm */}
      {(step === 'confirm' || step === 'done') && (
        <div className="space-y-4">
          <h3 className="text-sm font-semibold text-gray-700">
            {step === 'done' ? 'Node Added!' : 'Step 3: Confirm'}
          </h3>

          {step === 'done' ? (
            <div className="text-sm text-green-700 bg-green-50 rounded-lg p-3 flex items-center gap-2">
              <span className="text-green-600">&#10003;</span>
              Node <span className="font-medium">{nodeId}</span> has been registered.
            </div>
          ) : (
            <>
              <div>
                <label className="block text-xs font-medium text-gray-500 mb-1">Node ID</label>
                <input
                  type="text"
                  className="input w-full"
                  placeholder="my-esp32-node"
                  value={nodeId}
                  onChange={(e) => setNodeId(e.target.value)}
                />
                <p className="text-xs text-gray-400 mt-1">Unique identifier for this node. Auto-generated from detected board ID.</p>
              </div>

              {/* Summary */}
              <div className="text-sm bg-gray-50 rounded-lg p-3 space-y-1">
                <div className="flex justify-between">
                  <span className="text-gray-500">Type</span>
                  <span className="font-medium">{probeResult?.info?.platform || 'esp32'}</span>
                </div>
                <div className="flex justify-between">
                  <span className="text-gray-500">Transport</span>
                  <span className="font-medium">{transport === 'network' ? 'WiFi' : 'UART'}</span>
                </div>
                <div className="flex justify-between">
                  <span className="text-gray-500">Address</span>
                  <span className="font-medium font-mono text-xs">
                    {transport === 'network' ? `${host}:${port}` : `${serialPort} @ ${baud}`}
                  </span>
                </div>
                {probeResult?.info?.firmware_version && (
                  <div className="flex justify-between">
                    <span className="text-gray-500">Firmware</span>
                    <span className="font-medium">{probeResult.info.firmware_version}</span>
                  </div>
                )}
                {probeResult?.servo_scan && (
                  <div className="flex justify-between">
                    <span className="text-gray-500">Servos</span>
                    <span className="font-medium">{probeResult.servo_scan.count} found</span>
                  </div>
                )}
              </div>

              <div className="flex gap-2">
                <button
                  onClick={handleAdd}
                  disabled={!nodeId || adding}
                  className="btn-primary text-sm disabled:opacity-50"
                >
                  {adding ? 'Adding...' : 'Add Node'}
                </button>
                <button onClick={() => setStep('detect')} className="btn-secondary text-sm">
                  Back
                </button>
              </div>
            </>
          )}

          {step === 'done' && (
            <button onClick={onDone} className="btn-primary text-sm">Done</button>
          )}
        </div>
      )}
    </div>
  )
}

// ── Helper Components ──

function StepDot({ active, done, label }: { active: boolean; done: boolean; label: string }) {
  return (
    <div
      className={`w-7 h-7 rounded-full flex items-center justify-center text-xs font-bold transition-colors ${
        done ? 'bg-green-500 text-white' :
        active ? 'bg-servo-500 text-white' :
        'bg-gray-200 text-gray-500'
      }`}
    >
      {done ? '\u2713' : label}
    </div>
  )
}

function ProbeStep({ label, ok, error, detail }: { label: string; ok: boolean; error?: string; detail?: string }) {
  return (
    <div className="flex items-start gap-2 text-sm">
      <span className={`mt-0.5 flex-shrink-0 ${ok ? 'text-green-600' : 'text-red-500'}`}>
        {ok ? '\u2713' : '\u2717'}
      </span>
      <div>
        <span className={ok ? 'text-gray-700' : 'text-red-600'}>{label}</span>
        {detail && <span className="text-gray-400 ml-2">{detail}</span>}
        {error && <span className="text-red-400 ml-2 text-xs">{error}</span>}
      </div>
    </div>
  )
}
