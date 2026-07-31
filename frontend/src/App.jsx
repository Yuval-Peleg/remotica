import { BrowserRouter, Routes, Route } from "react-router-dom";
import { Navbar } from "@/components/Navbar";
import { Dashboard } from "@/pages/Dashboard";
import { Settings } from "@/pages/Settings";
import { PlaceholderPage } from "@/pages/Placeholder";

function App() {
  return (
    <BrowserRouter>
      <div className="min-h-screen bg-background text-foreground">
        <Navbar />
        <Routes>
          <Route path="/" element={<Dashboard />} />
          <Route path="/system" element={<PlaceholderPage title="System" />} />
          <Route path="/settings" element={<Settings />} />
          <Route path="/about" element={<PlaceholderPage title="About" />} />
        </Routes>
      </div>
    </BrowserRouter>
  );
}

export default App;
