import { Routes, Route } from 'react-router-dom'
import Layout from './components/Layout'
import Dashboard from './views/Dashboard'
import ServoControl from './views/ServoControl'
import Sensors from './views/Sensors'
import Settings from './views/Settings'
import Login from './views/Login'
import ApiDocs from './views/ApiDocs'

export default function App() {
  return (
    <Routes>
      <Route path="/login" element={<Login />} />
      <Route element={<Layout />}>
        <Route path="/" element={<Dashboard />} />
        <Route path="/servos" element={<ServoControl />} />
        <Route path="/sensors" element={<Sensors />} />
        <Route path="/settings" element={<Settings />} />
        <Route path="/docs" element={<ApiDocs />} />
      </Route>
    </Routes>
  )
}
