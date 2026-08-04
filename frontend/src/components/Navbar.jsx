import { Link, NavLink } from "react-router-dom";
import { Menu } from "lucide-react";
import { Button } from "@/components/ui/button";
import {
  Sheet,
  SheetContent,
  SheetHeader,
  SheetTitle,
  SheetTrigger,
  SheetClose,
} from "@/components/ui/sheet";
import { cn } from "@/lib/utils";

const navLinks = [
  { to: "/", label: "Home", end: true },
  { to: "/system", label: "System" },
  { to: "/settings", label: "Settings" },
  { to: "/about", label: "About" },
];

// The underline is an ::after bar that's always present but scaled to
// zero width, so showing it is a transform rather than a layout change —
// it grows out from the centre instead of the text jumping when a border
// appears and disappears.
function NavItem({ to, label, end, onNavigate }) {
  return (
    <NavLink
      to={to}
      end={end}
      onClick={onNavigate}
      className={({ isActive }) =>
        cn(
          "relative py-1 text-sm font-medium text-muted-foreground transition-colors duration-200 ease-soft hover:text-foreground",
          "after:absolute after:inset-x-0 after:-bottom-0.5 after:h-0.5 after:origin-center after:scale-x-0 after:rounded-full after:bg-primary after:transition-transform after:duration-300 after:ease-soft",
          "hover:after:scale-x-100",
          isActive && "text-foreground after:scale-x-100"
        )
      }
    >
      {label}
    </NavLink>
  );
}

export function Navbar() {
  return (
    <header className="sticky top-0 z-50 border-b border-border bg-background/95 backdrop-blur supports-backdrop-filter:bg-background/80">
      <nav className="mx-auto flex h-16 max-w-7xl items-center justify-between px-4 sm:px-6 lg:px-8">
        <Link
          to="/"
          className="flex items-center gap-2 transition-transform duration-300 ease-spring hover:scale-[1.03]"
        >
          <span className="bg-gradient-to-t from-brand-to to-brand-from bg-clip-text text-2xl font-bold tracking-wide text-transparent">
            REMOTICA
          </span>
        </Link>

        <div className="hidden items-center gap-6 md:flex">
          {navLinks.map((link) => (
            <NavItem key={link.to} {...link} />
          ))}
        </div>

        <Sheet>
          <SheetTrigger asChild>
            <Button variant="ghost" size="icon" className="md:hidden">
              <Menu />
              <span className="sr-only">Open menu</span>
            </Button>
          </SheetTrigger>
          <SheetContent side="right">
            <SheetHeader>
              <SheetTitle>Menu</SheetTitle>
            </SheetHeader>
            <div className="flex flex-col gap-1 px-4">
              {navLinks.map((link) => (
                <SheetClose asChild key={link.to}>
                  <NavLink
                    to={link.to}
                    end={link.end}
                    className={({ isActive }) =>
                      cn(
                        "rounded-md px-3 py-2 text-sm font-medium text-muted-foreground transition-all duration-200 ease-soft hover:translate-x-0.5 hover:bg-muted hover:text-foreground",
                        isActive && "bg-muted text-foreground"
                      )
                    }
                  >
                    {link.label}
                  </NavLink>
                </SheetClose>
              ))}
            </div>
          </SheetContent>
        </Sheet>
      </nav>
    </header>
  );
}
