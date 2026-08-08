import { BrowserRouter, Routes, Route, useLocation } from "react-router-dom";
import { Navbar } from "@/components/Navbar";
import { Dashboard } from "@/pages/Dashboard";
import { Settings } from "@/pages/Settings";
import { About } from "@/pages/About";
import { System } from "@/pages/System";
import { Toaster } from "@/components/ui/sonner";

// Keyed on the pathname so React tears down the old page and mounts the
// new one, which is what replays the entrance animation on every
// navigation — without the key it's the same element updating in place
// and nothing animates after the first load.
function AnimatedRoutes() {
  const location = useLocation();
  return (
    <div key={location.pathname} className="motion-page">
      <Routes location={location}>
        <Route path="/" element={<Dashboard />} />
        <Route path="/system" element={<System />} />
        <Route path="/settings" element={<Settings />} />
        <Route path="/about" element={<About />} />
      </Routes>
    </div>
  );
}

function App() {
  return (
    <BrowserRouter>
      <div className="min-h-screen bg-background text-foreground">
        <Navbar />
        <AnimatedRoutes />
      </div>
      <Toaster />
    </BrowserRouter>
  );
}

export default App;
