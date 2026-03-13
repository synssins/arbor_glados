/**
 * Servo Control & Configuration — comprehensive panel for Feetech STS3215 servos.
 * Covers all EEPROM/SRAM registers, live status, PID tuning, backup/restore, raw register view.
 * Task: W09
 */

import { useEffect, useState, useCallback, useRef } from 'react'
import { useServoStore } from '../stores/servo'
import { servo } from '../api/client'

// ── 16-bit LE helpers ──

function readU16LE(data: number[], addr: number): number {
  return (data[addr] ?? 0) | ((data[addr + 1] ?? 0) << 8)
}

function readS16LE(data: number[], addr: number): number {
  const u = readU16LE(data, addr)
  return u >= 0x8000 ? u - 0x10000 : u
}

function toU16LE(val: number): [number, number] {
  const v = val & 0xffff
  return [v & 0xff, (v >> 8) & 0xff]
}

// ── Constants ──

const BAUD_RATES: Record<number, string> = {
  0: '1 Mbps',
  1: '500 Kbps',
  2: '250 Kbps',
  3: '128 Kbps',
  4: '115200',
  5: '76800',
  6: '57600',
  7: '38400',
}

const MODE_LABELS: Record<number, string> = {
  0: 'Position Servo',
  1: 'Constant Speed Motor',
  2: 'PWM Motor',
  3: 'Step Mode',
}

const QUICK_POSITIONS = [0, 1024, 2048, 3072, 4095]

// ── Collapsible Section ──

function Section({
  title,
  defaultOpen = false,
  children,
}: {
  title: string
  defaultOpen?: boolean
  children: React.ReactNode
}) {
  const [open, setOpen] = useState(defaultOpen)
  return (
    <div className="border-t border-gray-100 pt-2">
      <button
        onClick={() => setOpen(!open)}
        className="flex items-center justify-between w-full text-left text-sm font-medium text-gray-700 hover:text-gray-900 py-1"
      >
        <span>{title}</span>
        <span className="text-xs text-gray-400">{open ? '\u25B2' : '\u25BC'}</span>
      </button>
      {open && <div className="mt-2 space-y-3">{children}</div>}
    </div>
  )
}

// ── Small reusable input row ──

function FieldRow({
  label,
  value,
  unit,
  type = 'number',
  min,
  max,
  step,
  readOnly,
  onChange,
}: {
  label: string
  value: string | number
  unit?: string
  type?: string
  min?: number
  max?: number
  step?: number
  readOnly?: boolean
  onChange?: (v: string) => void
}) {
  return (
    <div className="flex items-center gap-2">
      <label className="text-xs text-gray-500 w-32 shrink-0">{label}</label>
      <input
        type={type}
        className="input text-xs py-1 w-24"
        value={value}
        min={min}
        max={max}
        step={step}
        readOnly={readOnly}
        onChange={(e) => onChange?.(e.target.value)}
      />
      {unit && <span className="text-xs text-gray-400">{unit}</span>}
    </div>
  )
}

// ── Stat display cell ──

function Stat({ label, value, unit }: { label: string; value: string | number; unit?: string }) {
  return (
    <div>
      <span className="text-gray-500 text-xs">{label}</span>
      <div className="font-mono text-xs">
        {value}
        {unit ? <span className="text-gray-400 ml-0.5">{unit}</span> : null}
      </div>
    </div>
  )
}

// ── Per-servo card ──

function ServoCard({ id }: { id: number }) {
  const servoState = useServoStore((s) => s.servos.get(id))
  const setPosition = useServoStore((s) => s.setPosition)
  const setTorque = useServoStore((s) => s.setTorque)
  const fetchState = useServoStore((s) => s.fetchState)
  const scan = useServoStore((s) => s.scan)

  // Local UI state
  const [localPos, setLocalPos] = useState(servoState?.position ?? 2048)
  const [dragging, setDragging] = useState(false)

  // Registers cache
  const [regs, setRegs] = useState<number[] | null>(null)
  const [regsLoading, setRegsLoading] = useState(false)
  const [editRegs, setEditRegs] = useState<Record<number, string>>({})

  // Section-specific editable fields
  const [goalTime, setGoalTime] = useState('0')
  const [goalSpeed, setGoalSpeed] = useState('0')
  const [accel, setAccel] = useState('0')

  // Mode
  const [mode, setMode] = useState(0)

  // Speed/direction for modes 1 & 2
  const [wheelSpeed, setWheelSpeed] = useState(0) // magnitude
  const [wheelDir, setWheelDir] = useState<'cw' | 'ccw'>('cw')

  // PID
  const [pGain, setPGain] = useState('0')
  const [dGain, setDGain] = useState('0')
  const [iGain, setIGain] = useState('0')

  // Limits
  const [minAngle, setMinAngle] = useState('0')
  const [maxAngle, setMaxAngle] = useState('4095')
  const [maxTemp, setMaxTemp] = useState('70')
  const [maxVoltage, setMaxVoltage] = useState('85')
  const [minVoltage, setMinVoltage] = useState('50')
  const [maxTorque, setMaxTorque] = useState('1000')
  const [maxCurrent, setMaxCurrent] = useState('500')
  const [cwDeadBand, setCwDeadBand] = useState('0')
  const [ccwDeadBand, setCcwDeadBand] = useState('0')
  const [punch, setPunch] = useState('0')
  const [posOffset, setPosOffset] = useState('0')

  // Identity
  const [newId, setNewId] = useState('')
  const [idChanging, setIdChanging] = useState(false)
  const [idMsg, setIdMsg] = useState<string | null>(null)
  const [baudRate, setBaudRate] = useState(0)

  // Backup / Restore
  const [restoreFile, setRestoreFile] = useState<File | null>(null)
  const [backupMsg, setBackupMsg] = useState<string | null>(null)

  // Status messages
  const [writeMsg, setWriteMsg] = useState<string | null>(null)

  // Auto-refresh timer
  const refreshInterval = useRef<ReturnType<typeof setInterval> | null>(null)
  const [autoRefresh, setAutoRefresh] = useState(false)

  // Sync slider with store state
  useEffect(() => {
    if (!dragging && servoState) {
      setLocalPos(servoState.position)
    }
  }, [servoState?.position, dragging, servoState])

  // Auto-refresh
  useEffect(() => {
    if (autoRefresh) {
      refreshInterval.current = setInterval(() => fetchState(id), 500)
    }
    return () => {
      if (refreshInterval.current) clearInterval(refreshInterval.current)
    }
  }, [autoRefresh, id, fetchState])

  // Load registers and populate fields
  const loadRegisters = useCallback(async () => {
    setRegsLoading(true)
    try {
      const res = await servo.readRegisters(id)
      const d = res.data
      if (!d || !Array.isArray(d) || d.length < 70) {
        setWriteMsg('Read error: no register data returned (servo may be offline)')
        return
      }
      setRegs(d)

      // Populate fields from register data
      setGoalTime(String(readU16LE(d, 44)))
      setGoalSpeed(String(readU16LE(d, 46)))
      setAccel(String(d[41] ?? 0))
      setMode(d[33] ?? 0)
      setPGain(String(d[21] ?? 0))
      setDGain(String(d[22] ?? 0))
      setIGain(String(d[23] ?? 0))
      setMinAngle(String(readU16LE(d, 9)))
      setMaxAngle(String(readU16LE(d, 11)))
      setMaxTemp(String(d[13] ?? 70))
      setMaxVoltage(String(d[14] ?? 85))
      setMinVoltage(String(d[15] ?? 50))
      setMaxTorque(String(readU16LE(d, 16)))
      setMaxCurrent(String(readU16LE(d, 36)))
      setCwDeadBand(String(d[26] ?? 0))
      setCcwDeadBand(String(d[27] ?? 0))
      setPunch(String(readU16LE(d, 24)))
      setPosOffset(String(readS16LE(d, 33)))
      setBaudRate(d[6] ?? 0)
    } catch (e) {
      setWriteMsg(`Read error: ${(e as Error).message}`)
    } finally {
      setRegsLoading(false)
    }
  }, [id])

  // Load registers on mount
  useEffect(() => {
    loadRegisters()
  }, [loadRegisters])

  // ── Write helpers ──

  const writeSRAM = async (addr: number, data: number[]) => {
    setWriteMsg(null)
    try {
      await servo.writeRegister(id, addr, data, false)
      setWriteMsg('Written OK')
      setTimeout(() => setWriteMsg(null), 2000)
    } catch (e) {
      setWriteMsg(`Write error: ${(e as Error).message}`)
    }
  }

  const writeEEPROM = async (addr: number, data: number[]) => {
    setWriteMsg(null)
    try {
      await servo.writeRegister(id, addr, data, true)
      setWriteMsg('EEPROM written OK')
      setTimeout(() => setWriteMsg(null), 2000)
    } catch (e) {
      setWriteMsg(`EEPROM error: ${(e as Error).message}`)
    }
  }

  // ── Handlers ──

  const handleSliderChange = (e: React.ChangeEvent<HTMLInputElement>) => {
    setLocalPos(parseInt(e.target.value))
    setDragging(true)
  }

  const handleSliderRelease = () => {
    setDragging(false)
    setPosition(id, localPos)
  }

  const handleQuickPos = (pos: number) => {
    setLocalPos(pos)
    setPosition(id, pos)
  }

  const handleWriteMovement = async () => {
    // Write goal time (reg 44-45), goal speed (reg 46-47), acceleration (reg 41)
    await writeSRAM(41, [parseInt(accel) || 0])
    await writeSRAM(44, [...toU16LE(parseInt(goalTime) || 0)])
    await writeSRAM(46, [...toU16LE(parseInt(goalSpeed) || 0)])
  }

  const handleWriteWheelSpeed = async () => {
    // Bit 15 = direction (1=CCW), bits 0-14 = magnitude
    let raw = wheelSpeed & 0x7FFF
    if (wheelDir === 'ccw') raw |= 0x8000
    await writeSRAM(41, [parseInt(accel) || 0])
    await writeSRAM(46, [...toU16LE(raw)])
  }

  const handleStopWheel = async () => {
    await writeSRAM(46, [...toU16LE(0)])
  }

  const handleWriteMode = async () => {
    await writeEEPROM(33, [mode])
  }

  const handleWritePID = async () => {
    await writeEEPROM(21, [parseInt(pGain) || 0, parseInt(dGain) || 0, parseInt(iGain) || 0])
  }

  const handleWriteLimits = async () => {
    // Write all limit registers
    await writeEEPROM(9, [...toU16LE(parseInt(minAngle) || 0)])
    await writeEEPROM(11, [...toU16LE(parseInt(maxAngle) || 4095)])
    await writeEEPROM(13, [parseInt(maxTemp) || 70])
    await writeEEPROM(14, [parseInt(maxVoltage) || 85])
    await writeEEPROM(15, [parseInt(minVoltage) || 50])
    await writeEEPROM(16, [...toU16LE(parseInt(maxTorque) || 1000)])
    await writeEEPROM(24, [...toU16LE(parseInt(punch) || 0)])
    await writeEEPROM(26, [parseInt(cwDeadBand) || 0])
    await writeEEPROM(27, [parseInt(ccwDeadBand) || 0])
    await writeEEPROM(33, [...toU16LE(parseInt(posOffset) || 0)])
    await writeEEPROM(36, [...toU16LE(parseInt(maxCurrent) || 500)])
  }

  const handleChangeId = async () => {
    const parsedId = parseInt(newId)
    if (isNaN(parsedId) || parsedId < 0 || parsedId > 253) {
      setIdMsg('ID must be 0-253')
      return
    }
    if (!confirm(`Change servo ID from ${id} to ${parsedId}? This writes to EEPROM and is permanent.`)) return
    setIdChanging(true)
    setIdMsg(null)
    try {
      await servo.setId(id, parsedId)
      setIdMsg(`Changed to ID ${parsedId}`)
      setNewId('')
      scan()
    } catch (e) {
      setIdMsg(`Error: ${(e as Error).message}`)
    } finally {
      setIdChanging(false)
    }
  }

  const handleChangeBaud = async () => {
    if (!confirm(`Change baud rate to ${BAUD_RATES[baudRate]}? Servo will need matching baud on next connection.`)) return
    await writeEEPROM(6, [baudRate])
  }

  const handleBackup = async () => {
    setBackupMsg(null)
    try {
      const res = await servo.backup(id)
      const blob = new Blob(
        [JSON.stringify({ id: res.id, eeprom: res.eeprom, timestamp: new Date().toISOString() }, null, 2)],
        { type: 'application/json' },
      )
      const url = URL.createObjectURL(blob)
      const a = document.createElement('a')
      a.href = url
      a.download = `servo_${id}_backup.json`
      a.click()
      URL.revokeObjectURL(url)
      setBackupMsg('Backup downloaded')
    } catch (e) {
      setBackupMsg(`Backup error: ${(e as Error).message}`)
    }
  }

  const handleRestore = async () => {
    if (!restoreFile) return
    if (!confirm(`Restore EEPROM from file to servo #${id}? This will overwrite all settings.`)) return
    setBackupMsg(null)
    try {
      const text = await restoreFile.text()
      const parsed = JSON.parse(text)
      if (!Array.isArray(parsed.eeprom) || parsed.eeprom.length < 1) {
        setBackupMsg('Invalid backup file')
        return
      }
      const includeId = confirm('Also restore the servo ID from backup? (Cancel = keep current ID)')
      let newId: number | undefined
      if (includeId) {
        const backupId = parsed.eeprom[5] ?? parsed.id
        const input = prompt(
          `The backup contains ID ${backupId}. Enter the ID to assign to this servo (0-253):`,
          String(backupId),
        )
        if (input === null) return
        newId = parseInt(input)
        if (isNaN(newId) || newId < 0 || newId > 253) {
          setBackupMsg('Invalid ID (must be 0-253)')
          return
        }
      }
      await servo.restore(id, parsed.eeprom, includeId, newId)
      setBackupMsg('Restore complete' + (newId !== undefined ? ` — servo is now ID ${newId}` : ''))
      setRestoreFile(null)
      loadRegisters()
      if (includeId) scan()
    } catch (e) {
      setBackupMsg(`Restore error: ${(e as Error).message}`)
    }
  }

  const [resetting, setResetting] = useState(false)

  const handleFactoryReset = async () => {
    if (!confirm(`Reset servo #${id} to factory defaults? This will overwrite ALL EEPROM settings (except servo ID).`)) return
    setResetting(true)
    setBackupMsg(null)
    try {
      await servo.factoryReset(id)
      setBackupMsg('Factory reset complete — all EEPROM values restored to defaults')
      loadRegisters()
    } catch (e) {
      setBackupMsg(`Factory reset error: ${(e as Error).message}`)
    } finally {
      setResetting(false)
    }
  }

  // Skeleton while loading
  if (!servoState) {
    return (
      <div className="card animate-pulse">
        <div className="h-4 bg-gray-200 rounded w-24 mb-2" />
        <div className="h-8 bg-gray-200 rounded" />
      </div>
    )
  }

  return (
    <div className="card space-y-3">
      {/* ── Header ── */}
      <div className="flex items-center justify-between">
        <h3 className="font-semibold text-sm">
          Servo #{id}
          <span className="ml-2 text-xs font-normal text-gray-400">{MODE_LABELS[mode] ?? `Mode ${mode}`}</span>
        </h3>
        <div className="flex items-center gap-2">
          <label className="flex items-center gap-1 text-xs text-gray-500 cursor-pointer">
            <input
              type="checkbox"
              checked={autoRefresh}
              onChange={(e) => setAutoRefresh(e.target.checked)}
              className="w-3 h-3"
            />
            Auto
          </label>
          <button
            onClick={() => fetchState(id)}
            className="text-xs text-gray-500 hover:text-gray-700"
          >
            Refresh
          </button>
          <button
            onClick={() => setTorque(id, !servoState.torque_on)}
            className={`text-xs px-2 py-0.5 rounded ${
              servoState.torque_on ? 'bg-green-100 text-green-700' : 'bg-gray-100 text-gray-500'
            }`}
          >
            {servoState.torque_on ? 'Torque ON' : 'Torque OFF'}
          </button>
        </div>
      </div>

      {/* Write status message */}
      {writeMsg && (
        <div className={`text-xs px-2 py-1 rounded ${writeMsg.includes('error') || writeMsg.includes('Error') ? 'bg-red-50 text-red-600' : 'bg-green-50 text-green-600'}`}>
          {writeMsg}
        </div>
      )}

      {/* ── Section: Live Status ── */}
      <Section title={`Live Status — ${MODE_LABELS[mode] ?? 'Unknown'}`} defaultOpen={true}>
        {/* Position slider (mode 0 only) */}
        {mode === 0 && (
          <>
            <div>
              <div className="flex justify-between text-xs text-gray-500 mb-1">
                <span>Position</span>
                <span className="font-mono">{localPos}</span>
              </div>
              <input
                type="range"
                min={0}
                max={4095}
                value={localPos}
                onChange={handleSliderChange}
                onMouseUp={handleSliderRelease}
                onTouchEnd={handleSliderRelease}
                className="w-full h-2 bg-gray-200 rounded-lg appearance-none cursor-pointer accent-servo-600"
              />
              <div className="flex justify-between text-xs text-gray-400 mt-0.5">
                <span>0</span>
                <span>4095</span>
              </div>
            </div>
            <div className="flex gap-1">
              {QUICK_POSITIONS.map((pos) => (
                <button
                  key={pos}
                  onClick={() => handleQuickPos(pos)}
                  className="flex-1 text-xs py-1 rounded bg-gray-100 hover:bg-gray-200 transition-colors"
                >
                  {pos}
                </button>
              ))}
            </div>
          </>
        )}

        {/* Speed/direction control (modes 1 & 2) */}
        {(mode === 1 || mode === 2) && (
          <div className="space-y-2">
            <div>
              <div className="flex justify-between text-xs text-gray-500 mb-1">
                <span>{mode === 1 ? 'Speed (steps/s)' : 'PWM Duty (%)'}</span>
                <span className="font-mono">
                  {mode === 1 ? wheelSpeed : `${(wheelSpeed / 10).toFixed(1)}%`}
                  {' '}{wheelDir === 'cw' ? 'CW' : 'CCW'}
                </span>
              </div>
              <input
                type="range"
                min={0}
                max={mode === 1 ? 3400 : 1000}
                value={wheelSpeed}
                onChange={(e) => setWheelSpeed(parseInt(e.target.value))}
                onMouseUp={handleWriteWheelSpeed}
                onTouchEnd={handleWriteWheelSpeed}
                className="w-full h-2 bg-gray-200 rounded-lg appearance-none cursor-pointer accent-servo-600"
              />
              <div className="flex justify-between text-xs text-gray-400 mt-0.5">
                <span>0</span>
                <span>{mode === 1 ? '3400' : '100%'}</span>
              </div>
            </div>
            <div className="flex items-center gap-2">
              <button
                onClick={() => { setWheelDir('cw'); handleWriteWheelSpeed() }}
                className={`flex-1 text-xs py-1.5 rounded font-medium ${wheelDir === 'cw' ? 'bg-blue-100 text-blue-700 ring-1 ring-blue-300' : 'bg-gray-100 text-gray-600'}`}
              >
                CW
              </button>
              <button
                onClick={handleStopWheel}
                className="flex-1 text-xs py-1.5 rounded font-medium bg-red-100 text-red-700"
              >
                STOP
              </button>
              <button
                onClick={() => { setWheelDir('ccw'); handleWriteWheelSpeed() }}
                className={`flex-1 text-xs py-1.5 rounded font-medium ${wheelDir === 'ccw' ? 'bg-blue-100 text-blue-700 ring-1 ring-blue-300' : 'bg-gray-100 text-gray-600'}`}
              >
                CCW
              </button>
            </div>
            <FieldRow label="Acceleration" value={accel} min={0} max={255} onChange={setAccel} />
            {mode === 1 && (
              <div className="text-xs text-gray-400">
                ~{(wheelSpeed * 0.732 / 50).toFixed(1)} RPM (50 steps/s = 0.732 RPM)
              </div>
            )}
          </div>
        )}

        {/* Stats grid */}
        <div className="grid grid-cols-3 gap-2 text-xs">
          {mode === 0 && <Stat label="Speed" value={servoState.speed} />}
          {(mode === 1 || mode === 2) && (
            <Stat
              label="Actual Speed"
              value={Math.abs(servoState.speed)}
              unit={servoState.speed < 0 ? 'CCW' : 'CW'}
            />
          )}
          <Stat label="Load" value={servoState.load} />
          <Stat label="Temp" value={servoState.temperature} unit="°C" />
          <Stat label="Voltage" value={(servoState.voltage / 10).toFixed(1)} unit="V" />
          <Stat
            label="Current"
            value={regs ? readU16LE(regs, 69) : '—'}
            unit="mA"
          />
          <Stat
            label="Moving"
            value={regs ? (regs[66] ? 'Yes' : 'No') : '—'}
          />
        </div>
      </Section>

      {/* ── Section: Movement Control (mode 0 only) ── */}
      {mode === 0 && (
        <Section title="Movement Control">
          <div className="space-y-2">
            <FieldRow label="Goal Position" value={localPos} min={0} max={4095} onChange={(v) => { setLocalPos(parseInt(v) || 0); setPosition(id, parseInt(v) || 0) }} />
            <FieldRow label="Goal Time" value={goalTime} min={0} max={65535} unit="ms" onChange={setGoalTime} />
            <FieldRow label="Goal Speed" value={goalSpeed} min={0} max={65535} unit="steps/s" onChange={setGoalSpeed} />
            <FieldRow label="Acceleration" value={accel} min={0} max={255} onChange={setAccel} />
            <button onClick={handleWriteMovement} className="btn-primary text-xs">
              Write Movement Params
            </button>
          </div>
        </Section>
      )}

      {/* ── Section: Mode ── */}
      <Section title="Mode">
        <div className="flex items-center gap-2">
          <select
            className="input text-xs py-1 w-48"
            value={mode}
            onChange={(e) => setMode(parseInt(e.target.value))}
          >
            {Object.entries(MODE_LABELS).map(([val, label]) => (
              <option key={val} value={val}>{label}</option>
            ))}
          </select>
          <button onClick={handleWriteMode} className="btn-primary text-xs">
            Write (EEPROM)
          </button>
        </div>
        <p className="text-xs text-gray-400">Current register value: {regs ? regs[33] : '—'}</p>
      </Section>

      {/* ── Section: PID Tuning ── */}
      <Section title="PID Tuning">
        <div className="space-y-2">
          <FieldRow label="P Gain" value={pGain} min={0} max={255} onChange={setPGain} />
          <FieldRow label="D Gain" value={dGain} min={0} max={255} onChange={setDGain} />
          <FieldRow label="I Gain" value={iGain} min={0} max={255} onChange={setIGain} />
          <button onClick={handleWritePID} className="btn-primary text-xs">
            Write PID (EEPROM)
          </button>
        </div>
      </Section>

      {/* ── Section: Limits ── */}
      <Section title="Limits &amp; Protection">
        <div className="space-y-2">
          <FieldRow label="Min Angle" value={minAngle} min={0} max={4095} onChange={setMinAngle} />
          <FieldRow label="Max Angle" value={maxAngle} min={0} max={4095} onChange={setMaxAngle} />
          <FieldRow label="Max Temp" value={maxTemp} min={0} max={255} unit="°C" onChange={setMaxTemp} />
          <FieldRow label="Max Voltage" value={maxVoltage} min={0} max={255} unit="(0.1V)" onChange={setMaxVoltage} />
          <FieldRow label="Min Voltage" value={minVoltage} min={0} max={255} unit="(0.1V)" onChange={setMinVoltage} />
          <FieldRow label="Max Torque" value={maxTorque} min={0} max={1000} unit="(permil)" onChange={setMaxTorque} />
          <FieldRow label="Max Current" value={maxCurrent} min={0} max={65535} unit="mA" onChange={setMaxCurrent} />
          <FieldRow label="CW Dead Band" value={cwDeadBand} min={0} max={255} onChange={setCwDeadBand} />
          <FieldRow label="CCW Dead Band" value={ccwDeadBand} min={0} max={255} onChange={setCcwDeadBand} />
          <FieldRow label="Punch (Min Force)" value={punch} min={0} max={65535} onChange={setPunch} />
          <FieldRow label="Position Offset" value={posOffset} min={-32768} max={32767} onChange={setPosOffset} />
          <button onClick={handleWriteLimits} className="btn-primary text-xs">
            Write All Limits (EEPROM)
          </button>
        </div>
      </Section>

      {/* ── Section: Identity ── */}
      <Section title="Identity">
        <div className="space-y-3">
          {/* ID change */}
          <div className="flex items-center gap-2">
            <span className="text-xs text-gray-500 w-32 shrink-0">Change ID (current: {id})</span>
            <input
              type="number"
              min={0}
              max={253}
              value={newId}
              onChange={(e) => setNewId(e.target.value)}
              placeholder="New ID"
              className="input text-xs py-1 w-20"
              onKeyDown={(e) => e.key === 'Enter' && handleChangeId()}
            />
            <button onClick={handleChangeId} disabled={idChanging} className="btn-primary text-xs">
              {idChanging ? '...' : 'Set ID'}
            </button>
          </div>
          {idMsg && <p className="text-xs text-gray-600">{idMsg}</p>}

          {/* Baud rate */}
          <div className="flex items-center gap-2">
            <span className="text-xs text-gray-500 w-32 shrink-0">Baud Rate</span>
            <select
              className="input text-xs py-1 w-32"
              value={baudRate}
              onChange={(e) => setBaudRate(parseInt(e.target.value))}
            >
              {Object.entries(BAUD_RATES).map(([val, label]) => (
                <option key={val} value={val}>{label}</option>
              ))}
            </select>
            <button onClick={handleChangeBaud} className="btn-primary text-xs">
              Write (EEPROM)
            </button>
          </div>

          {/* Read-only info from registers */}
          {regs && (
            <div className="text-xs text-gray-400 space-y-0.5">
              <div>Model: {readU16LE(regs, 3)}</div>
              <div>Return Delay: {regs[7]} us</div>
              <div>Response Status: {regs[8]}</div>
              <div>Alarm LED: 0x{(regs[19] ?? 0).toString(16).padStart(2, '0')}</div>
              <div>Alarm Shutdown: 0x{(regs[20] ?? 0).toString(16).padStart(2, '0')}</div>
              <div>EEPROM Lock: {regs[55] ? 'Locked' : 'Unlocked'}</div>
            </div>
          )}
        </div>
      </Section>

      {/* ── Section: Backup / Restore ── */}
      <Section title="Backup / Restore">
        <div className="space-y-2">
          <button onClick={handleBackup} className="btn-secondary text-xs">
            Download Backup (JSON)
          </button>
          <div className="flex items-center gap-2">
            <input
              type="file"
              accept=".json"
              className="text-xs"
              onChange={(e) => setRestoreFile(e.target.files?.[0] ?? null)}
            />
            <button
              onClick={handleRestore}
              disabled={!restoreFile}
              className="btn-danger text-xs"
            >
              Restore
            </button>
          </div>
          <div className="border-t border-gray-100 pt-2 mt-2">
            <button
              onClick={handleFactoryReset}
              disabled={resetting}
              className="btn-danger text-xs"
            >
              {resetting ? 'Resetting...' : 'Reset to Factory Defaults'}
            </button>
            <p className="text-xs text-gray-400 mt-1">
              Writes all EEPROM registers (6–39) to ST3215 factory values. Servo ID is preserved.
            </p>
          </div>
          {backupMsg && <p className="text-xs text-gray-600">{backupMsg}</p>}
        </div>
      </Section>

      {/* ── Section: Registers ── */}
      <Section title="Registers">
        <div className="space-y-2">
          <div className="flex gap-2 items-center">
            <button onClick={loadRegisters} disabled={regsLoading} className="btn-secondary text-xs">
              {regsLoading ? 'Reading...' : 'Refresh Registers'}
            </button>
            <span className="text-xs text-gray-400">
              <span className="text-blue-400">EPROM</span> = persistent &nbsp;
              <span className="text-amber-400">SRAM</span> = volatile
            </span>
          </div>
          {regs && (
            <div className="overflow-x-auto">
              <table className="text-xs font-mono w-full">
                <thead>
                  <tr className="text-left text-gray-400 border-b border-gray-200">
                    <th className="pr-1 py-1 w-8">Addr</th>
                    <th className="pr-1 py-1 w-10">Hex</th>
                    <th className="pr-1 py-1 w-16">Value</th>
                    <th className="pr-1 py-1 w-28">Name</th>
                    <th className="pr-1 py-1 w-32">Help</th>
                    <th className="py-1 w-12"></th>
                  </tr>
                </thead>
                <tbody>
                  {Object.entries(REGISTER_MAP).map(([addrStr, info]) => {
                    const addr = Number(addrStr)
                    const isWritable = info.access === 'rw'
                    const isEprom = info.storage === 'EPROM'
                    const val = info.bytes === 2 ? readU16LE(regs, addr) : (regs[addr] ?? 0)
                    const editVal = editRegs[addr]
                    const hasEdit = editVal !== undefined && editVal !== ''
                    const storageColor = isEprom ? 'text-blue-400' : 'text-amber-400'
                    const rangeStr = info.min !== undefined ? `${info.min}–${info.max}` : ''
                    const unitStr = info.unit ? ` ${info.unit}` : ''

                    return (
                      <tr key={addr} className="border-t border-gray-50 hover:bg-gray-50">
                        <td className="pr-1 py-0.5 text-gray-400">{addr}</td>
                        <td className="pr-1 py-0.5">0x{val.toString(16).padStart(info.bytes === 2 ? 4 : 2, '0')}</td>
                        <td className="pr-1 py-0.5">
                          {isWritable ? (
                            <input
                              type="number"
                              className="w-16 px-1 py-0 border border-gray-200 rounded text-xs font-mono"
                              placeholder={String(val)}
                              value={editVal ?? ''}
                              onChange={(e) => setEditRegs(prev => ({ ...prev, [addr]: e.target.value }))}
                              min={info.min}
                              max={info.max}
                            />
                          ) : (
                            <span>{val}</span>
                          )}
                        </td>
                        <td className="pr-1 py-0.5">
                          <span className={storageColor}>{info.name}</span>
                          {info.dflt !== undefined && <span className="text-gray-300 ml-1">(def:{info.dflt})</span>}
                        </td>
                        <td className="pr-1 py-0.5 text-gray-400 text-[10px] leading-tight" title={info.help}>
                          {info.help}
                          {rangeStr && <span className="block text-gray-300">[{rangeStr}]{unitStr}</span>}
                        </td>
                        <td className="py-0.5">
                          {isWritable && hasEdit && (
                            <button
                              className="btn-secondary text-[10px] px-1 py-0"
                              onClick={async () => {
                                const numVal = parseInt(editVal)
                                if (isNaN(numVal)) return
                                const data = info.bytes === 2 ? [...toU16LE(numVal)] : [numVal]
                                if (isEprom) {
                                  await writeEEPROM(addr, data)
                                } else {
                                  await writeSRAM(addr, data)
                                }
                                setEditRegs(prev => { const next = { ...prev }; delete next[addr]; return next })
                                loadRegisters()
                              }}
                            >
                              {isEprom ? 'Write EEPROM' : 'Write'}
                            </button>
                          )}
                        </td>
                      </tr>
                    )
                  })}
                </tbody>
              </table>
            </div>
          )}
        </div>
      </Section>
    </div>
  )
}

// ── ST3215 Register Metadata — from official memory table v3.6 ──

interface RegInfo {
  name: string
  bytes: number         // 1 or 2 (2-byte regs: this addr = low byte, next = high byte)
  storage: 'EPROM' | 'SRAM'
  access: 'r' | 'rw'
  min?: number
  max?: number
  unit?: string
  dflt?: number         // factory default
  help: string
}

const REGISTER_MAP: Record<number, RegInfo> = {
  0:  { name: 'FW Major',        bytes: 1, storage: 'EPROM', access: 'r',  dflt: 3,    help: 'Firmware major version' },
  1:  { name: 'FW Minor',        bytes: 1, storage: 'EPROM', access: 'r',  dflt: 6,    help: 'Firmware minor version' },
  3:  { name: 'Model Major',     bytes: 1, storage: 'EPROM', access: 'r',  dflt: 9,    help: 'Servo model major version' },
  4:  { name: 'Model Minor',     bytes: 1, storage: 'EPROM', access: 'r',  dflt: 3,    help: 'Servo model minor version' },
  5:  { name: 'ID',              bytes: 1, storage: 'EPROM', access: 'rw', min: 0, max: 253, dflt: 1,    help: 'Bus ID. 254=broadcast. No duplicates!' },
  6:  { name: 'Baud Rate',       bytes: 1, storage: 'EPROM', access: 'rw', min: 0, max: 7,   dflt: 0,    help: '0=1M 1=500K 2=250K 3=128K 4=115200 5=76800 6=57600 7=38400' },
  7:  { name: 'Return Delay',    bytes: 1, storage: 'EPROM', access: 'rw', min: 0, max: 254,  dflt: 0,    unit: '2\u00B5s', help: 'Response delay. Max 508\u00B5s' },
  8:  { name: 'Response Level',  bytes: 1, storage: 'EPROM', access: 'rw', min: 0, max: 1,   dflt: 1,    help: '0=reply to READ/PING only; 1=reply to all' },
  9:  { name: 'Min Angle',       bytes: 2, storage: 'EPROM', access: 'rw', min: 0, max: 4094, dflt: 0,    unit: 'step', help: 'Min motion limit. Set 0 for multi-turn' },
  11: { name: 'Max Angle',       bytes: 2, storage: 'EPROM', access: 'rw', min: 1, max: 4095, dflt: 4095, unit: 'step', help: 'Max motion limit. Set 0 for multi-turn' },
  13: { name: 'Max Temp',        bytes: 1, storage: 'EPROM', access: 'rw', min: 0, max: 100,  dflt: 70,   unit: '\u00B0C', help: 'Max operating temperature limit' },
  14: { name: 'Max Voltage',     bytes: 1, storage: 'EPROM', access: 'rw', min: 0, max: 254,  dflt: 80,   unit: '0.1V', help: 'Max input voltage (80 = 8.0V)' },
  15: { name: 'Min Voltage',     bytes: 1, storage: 'EPROM', access: 'rw', min: 0, max: 254,  dflt: 40,   unit: '0.1V', help: 'Min input voltage (40 = 4.0V)' },
  16: { name: 'Max Torque',      bytes: 2, storage: 'EPROM', access: 'rw', min: 0, max: 1000, dflt: 1000, unit: '0.1%', help: '1000 = 100% stall torque. Assigned to addr 48 on boot' },
  18: { name: 'Phase',           bytes: 1, storage: 'EPROM', access: 'rw', min: 0, max: 254,  dflt: 12,   help: 'Special function byte. Do not modify!' },
  19: { name: 'Unloading Cond',  bytes: 1, storage: 'EPROM', access: 'rw', min: 0, max: 254,  dflt: 44,   help: 'Protection enables. Bits: 0=V 1=sensor 2=temp 3=current 4=angle 5=overload' },
  20: { name: 'LED Alarm Cond',  bytes: 1, storage: 'EPROM', access: 'rw', min: 0, max: 254,  dflt: 47,   help: 'LED flash alarm enables. Same bit layout as addr 19' },
  21: { name: 'P Gain',          bytes: 1, storage: 'EPROM', access: 'rw', min: 0, max: 254,  dflt: 32,   help: 'Position PID proportional coefficient' },
  22: { name: 'D Gain',          bytes: 1, storage: 'EPROM', access: 'rw', min: 0, max: 254,  dflt: 32,   help: 'Position PID differential coefficient' },
  23: { name: 'I Gain',          bytes: 1, storage: 'EPROM', access: 'rw', min: 0, max: 254,  dflt: 0,    help: 'Position PID integral coefficient' },
  24: { name: 'Min Startup Force', bytes: 2, storage: 'EPROM', access: 'rw', min: 0, max: 1000, dflt: 16, unit: '0.1%', help: 'Min output starting torque. 1000=100%' },
  26: { name: 'CW Dead Band',    bytes: 1, storage: 'EPROM', access: 'rw', min: 0, max: 32,   dflt: 1,    unit: 'step', help: 'Clockwise insensitive zone' },
  27: { name: 'CCW Dead Band',   bytes: 1, storage: 'EPROM', access: 'rw', min: 0, max: 32,   dflt: 1,    unit: 'step', help: 'Counter-clockwise insensitive zone' },
  28: { name: 'Protect Current', bytes: 2, storage: 'EPROM', access: 'rw', min: 0, max: 511,  dflt: 500,  unit: '6.5mA', help: 'Max current threshold. 500 = 3250mA' },
  30: { name: 'Angular Res',     bytes: 1, storage: 'EPROM', access: 'rw', min: 1, max: 100,  dflt: 1,    help: 'Resolution multiplier. Extends turn count' },
  31: { name: 'Pos Correction',  bytes: 2, storage: 'EPROM', access: 'rw', min: -2047, max: 2047, dflt: 0, unit: 'step', help: 'Position offset. Bit11=direction' },
  33: { name: 'Mode',            bytes: 1, storage: 'EPROM', access: 'rw', min: 0, max: 3,    dflt: 0,    help: '0=Position 1=Constant Speed 2=PWM 3=Step' },
  34: { name: 'Protect Torque',  bytes: 1, storage: 'EPROM', access: 'rw', min: 0, max: 254,  dflt: 20,   unit: '%', help: 'Output torque after overload protection triggers' },
  35: { name: 'Protect Time',    bytes: 1, storage: 'EPROM', access: 'rw', min: 0, max: 254,  dflt: 200,  unit: '10ms', help: 'Overload timing. 200=2s, max 2.54s' },
  36: { name: 'Overload Torque', bytes: 1, storage: 'EPROM', access: 'rw', min: 0, max: 254,  dflt: 80,   unit: '%', help: 'Threshold to start overload timer. 80=80%' },
  37: { name: 'Speed P Gain',    bytes: 1, storage: 'EPROM', access: 'rw', min: 0, max: 254,  dflt: 10,   help: 'Speed loop P coefficient (Mode 1)' },
  38: { name: 'Overcurrent Time', bytes: 1, storage: 'EPROM', access: 'rw', min: 0, max: 254, dflt: 200,  unit: '10ms', help: 'Max 254*10ms = 2540ms' },
  39: { name: 'Speed I Gain',    bytes: 1, storage: 'EPROM', access: 'rw', min: 0, max: 254,  dflt: 10,   help: 'Speed loop I coefficient (Mode 1)' },
  40: { name: 'Torque Enable',   bytes: 1, storage: 'SRAM',  access: 'rw', min: 0, max: 128,  dflt: 0,    help: '0=off 1=on 128=calibrate pos to 2048' },
  41: { name: 'Acceleration',    bytes: 1, storage: 'SRAM',  access: 'rw', min: 0, max: 254,  dflt: 0,    unit: '100 step/s\u00B2', help: '10 = 1000 step/s\u00B2 accel/decel' },
  42: { name: 'Goal Position',   bytes: 2, storage: 'SRAM',  access: 'rw', min: -32766, max: 32766, dflt: 0, unit: 'step', help: 'Target position (absolute)' },
  44: { name: 'Goal Time',       bytes: 2, storage: 'SRAM',  access: 'rw', min: 0, max: 1000, dflt: 0,    unit: 'ms', help: 'Movement time' },
  46: { name: 'Goal Speed',      bytes: 2, storage: 'SRAM',  access: 'rw', min: 0, max: 254,  dflt: 0,    unit: 'step/s', help: '50 step/s = 0.732 RPM' },
  48: { name: 'Torque Limit',    bytes: 2, storage: 'SRAM',  access: 'rw', min: 0, max: 1000, dflt: 1000, unit: '0.1%', help: 'Initialized from Max Torque (addr 16) on boot' },
  55: { name: 'EEPROM Lock',     bytes: 1, storage: 'SRAM',  access: 'rw', min: 0, max: 1,    dflt: 0,    help: '0=EEPROM writes persist; 1=EEPROM writes volatile' },
  56: { name: 'Present Pos',     bytes: 2, storage: 'SRAM',  access: 'r',  help: 'Current position feedback' },
  58: { name: 'Present Speed',   bytes: 2, storage: 'SRAM',  access: 'r',  unit: 'step/s', help: 'Current speed feedback' },
  60: { name: 'Present Load',    bytes: 2, storage: 'SRAM',  access: 'r',  unit: '0.1%', help: 'Drive voltage duty cycle' },
  62: { name: 'Present Voltage', bytes: 1, storage: 'SRAM',  access: 'r',  unit: '0.1V', help: 'Current supply voltage' },
  63: { name: 'Present Temp',    bytes: 1, storage: 'SRAM',  access: 'r',  unit: '\u00B0C', help: 'Internal temperature' },
  64: { name: 'Async Write Flag', bytes: 1, storage: 'SRAM', access: 'r',  help: 'REG_WRITE pending flag' },
  65: { name: 'Servo Status',    bytes: 1, storage: 'SRAM',  access: 'r',  help: 'Error bits: 0=V 1=sensor 2=temp 3=current 4=angle 5=overload' },
  66: { name: 'Moving',          bytes: 1, storage: 'SRAM',  access: 'r',  help: '1=moving, 0=stopped' },
  69: { name: 'Present Current', bytes: 2, storage: 'SRAM',  access: 'r',  unit: '6.5mA', help: 'Max measurable: 500*6.5=3250mA' },
}

// ── Main view ──

export default function ServoControl() {
  const scannedIds = useServoStore((s) => s.scannedIds)
  const scanning = useServoStore((s) => s.scanning)
  const scan = useServoStore((s) => s.scan)
  const error = useServoStore((s) => s.error)
  const [manualId, setManualId] = useState('')

  // Auto-scan on mount if no servos found yet
  useEffect(() => {
    if (scannedIds.length === 0 && !scanning) {
      scan()
    }
  }, []) // eslint-disable-line react-hooks/exhaustive-deps

  const handleAddManual = useCallback(() => {
    const id = parseInt(manualId)
    if (!isNaN(id) && id >= 0 && id <= 253) {
      useServoStore.getState().fetchState(id)
      if (!scannedIds.includes(id)) {
        useServoStore.setState((s) => ({
          scannedIds: [...s.scannedIds, id].sort((a, b) => a - b),
        }))
      }
      setManualId('')
    }
  }, [manualId, scannedIds])

  return (
    <div className="space-y-6">
      {/* ── Top bar ── */}
      <div className="flex items-center justify-between flex-wrap gap-2">
        <h2 className="text-xl font-bold">Servo Control</h2>
        <div className="flex items-center gap-2">
          <input
            type="number"
            min={0}
            max={253}
            value={manualId}
            onChange={(e) => setManualId(e.target.value)}
            placeholder="ID"
            className="input w-20"
            onKeyDown={(e) => e.key === 'Enter' && handleAddManual()}
          />
          <button onClick={handleAddManual} className="btn-secondary">
            Add
          </button>
          <button onClick={scan} disabled={scanning} className="btn-primary">
            {scanning ? 'Scanning...' : 'Scan Bus'}
          </button>
        </div>
      </div>

      {/* Scanned IDs chip bar */}
      {scannedIds.length > 0 && (
        <div className="flex flex-wrap gap-1">
          {scannedIds.map((sid) => (
            <span
              key={sid}
              className="text-xs px-2 py-0.5 rounded-full bg-servo-100 text-servo-700 font-mono"
            >
              #{sid}
            </span>
          ))}
        </div>
      )}

      {error && (
        <div className="bg-red-50 text-red-700 text-sm px-3 py-2 rounded">
          {error}
        </div>
      )}

      {scannedIds.length === 0 && !scanning && (
        <div className="text-center text-gray-500 py-12">
          No servos found. Click &quot;Scan Bus&quot; or add a servo ID manually.
        </div>
      )}

      {/* ── Servo cards ── */}
      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
        {scannedIds.map((id) => (
          <ServoCard key={id} id={id} />
        ))}
      </div>
    </div>
  )
}
