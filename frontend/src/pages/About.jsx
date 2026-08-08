import {
  CircuitBoard,
  Cpu,
  ExternalLink,
  HardDriveDownload,
  ShieldAlert,
  TriangleAlert,
  Wifi,
} from "lucide-react";
import { Card, CardContent } from "@/components/ui/card";
import { Alert, AlertDescription, AlertTitle } from "@/components/ui/alert";
import { Badge } from "@/components/ui/badge";
import { Separator } from "@/components/ui/separator";

const REPO_URL = "https://github.com/Yuval-Peleg/remotica";

const PRINCIPLES = [
  {
    icon: HardDriveDownload,
    title: "Runs on your bench",
    body: "Remotica lives on the machine that's physically wired to the printer. You open it from your phone or laptop over the local network — there's no account to make and no server of anyone else's in the path.",
  },
  {
    icon: CircuitBoard,
    title: "Talks to the printer itself",
    body: "Commands go out over USB as plain G-code, the same language a printer's own screen speaks. Remotica reads the firmware's replies back — temperatures, acknowledgements, resend requests — rather than guessing at what the machine is doing.",
  },
  {
    icon: Wifi,
    title: "Stays on your network",
    body: "Nothing is uploaded anywhere. G-code files sit on the machine you put them on, the camera stream never leaves the LAN, and no port needs forwarding to the internet for any of it to work.",
  },
];

const WORKING = [
  "Connecting to a printer over USB, including finding the port on its own",
  "Reading live temperatures, and setting hotend and bed targets",
  "Identifying some printers automatically from their firmware reply",
  "Uploading G-code, keeping it on the device, and starting a print",
  "Jogging and homing from the control panel",
  "An optional webcam view, hot-pluggable and confirmed capturing frames",
];

const UNVERIFIED = [
  "A full print, start to finish, on real hardware",
  "Jog, home, and temperature commands against a physical printer",
  "The checksum and resend path a long print depends on",
  "Noticing a printer that disappears mid-session",
  "Any kind of access control — anyone on your network can drive it",
];

const STACK = [
  {
    label: "Backend",
    items: [
      ["C", "no framework, no runtime"],
      ["civetweb", "embedded HTTP + WebSocket"],
      ["cJSON", "JSON parsing"],
      ["POSIX termios", "serial link to the printer"],
      ["V4L2", "webcam capture"],
    ],
  },
  {
    label: "Frontend",
    items: [
      ["React", "with Vite"],
      ["Tailwind CSS", "sage-on-charcoal, dark only"],
      ["shadcn/ui", "component primitives"],
      ["Lucide", "icons"],
    ],
  },
];

function PrincipleCard({ icon: Icon, title, body }) {
  return (
    <Card className="h-full transition-shadow duration-300 ease-soft hover:ring-primary/30">
      <CardContent className="flex h-full flex-col gap-2">
        <Icon className="size-5 text-primary" />
        <h3 className="font-heading text-base font-medium text-foreground">
          {title}
        </h3>
        <p className="text-sm leading-relaxed text-muted-foreground">{body}</p>
      </CardContent>
    </Card>
  );
}

function StatusList({ title, tone, items }) {
  return (
    <div className="flex flex-col gap-3">
      <h3 className="font-heading text-sm font-medium text-foreground">
        {title}
      </h3>
      <ul className="flex flex-col gap-2">
        {items.map((item) => (
          <li key={item} className="flex items-start gap-2.5 text-sm">
            <span
              className={
                tone === "good"
                  ? "mt-1.5 size-1.5 shrink-0 rounded-full bg-primary"
                  : "mt-1.5 size-1.5 shrink-0 rounded-full bg-amber-500"
              }
            />
            <span className="text-muted-foreground">{item}</span>
          </li>
        ))}
      </ul>
    </div>
  );
}

export function About() {
  return (
    <main className="mx-auto flex max-w-4xl flex-col gap-10 px-4 py-12 sm:px-6 lg:px-8">
      <header className="flex flex-col items-start gap-4 animate-rise">
        <Badge variant="secondary" className="gap-1.5">
          <Cpu className="size-3" />
          Work in progress
        </Badge>
        <h1 className="font-heading text-4xl font-bold tracking-tight text-foreground sm:text-5xl">
          A dashboard for the printer{" "}
          <span className="bg-gradient-to-t from-brand-to to-brand-from bg-clip-text text-transparent">
            in the other room
          </span>
        </h1>
        <p className="max-w-2xl text-base leading-relaxed text-muted-foreground">
          Remotica is a self-hosted remote control panel for 3D printers, built
          from scratch and inspired by{" "}
          <a
            href="https://octoprint.org/"
            target="_blank"
            rel="noreferrer"
            className="text-foreground underline decoration-primary/40 underline-offset-4 transition-colors duration-200 ease-soft hover:decoration-primary"
          >
            OctoPrint
          </a>
          . Point a browser at the machine wired to your printer and you get
          temperatures, movement, the file you&apos;re printing, and a camera —
          from wherever you actually are.
        </p>
      </header>

      <Alert variant="destructive" className="animate-rise">
        <TriangleAlert />
        <AlertTitle>Don&apos;t leave this running a real print yet</AlertTitle>
        <AlertDescription>
          This project drives heaters and motors, and the code that does so has
          only been partly tested against a physical printer. A bug in it could
          damage hardware or start a fire. Treat it as something to experiment
          with while you&apos;re standing next to the machine — not as something
          to trust with an unattended print.
        </AlertDescription>
      </Alert>

      <section className="grid gap-4 motion-stagger sm:grid-cols-2 lg:grid-cols-3">
        {PRINCIPLES.map((principle) => (
          <PrincipleCard key={principle.title} {...principle} />
        ))}
      </section>

      <section className="flex flex-col gap-5 animate-rise">
        <div className="flex flex-col gap-1">
          <h2 className="font-heading text-xl font-semibold text-foreground">
            Where it stands
          </h2>
          <p className="text-sm text-muted-foreground">
            The honest version — what has actually been exercised against a
            printer, and what is so far only written and reviewed.
          </p>
        </div>
        <Card>
          <CardContent className="grid gap-8 sm:grid-cols-2">
            <StatusList title="Working" tone="good" items={WORKING} />
            <StatusList
              title="Not verified yet"
              tone="warn"
              items={UNVERIFIED}
            />
          </CardContent>
        </Card>
      </section>

      <section className="flex flex-col gap-5 animate-rise">
        <div className="flex flex-col gap-1">
          <h2 className="font-heading text-xl font-semibold text-foreground">
            Under the hood
          </h2>
          <p className="text-sm text-muted-foreground">
            The backend is C on purpose, not as a placeholder. Its two
            dependencies are vendored as source and compiled straight into the
            binary — there's no package manager to keep happy on the machine
            that's supposed to just sit there and run.
          </p>
        </div>
        <div className="grid gap-4 motion-stagger sm:grid-cols-2">
          {STACK.map(({ label, items }) => (
            <Card key={label}>
              <CardContent className="flex flex-col gap-3">
                <span className="text-xs font-medium tracking-wide text-muted-foreground uppercase">
                  {label}
                </span>
                <dl className="flex flex-col gap-2">
                  {items.map(([name, note]) => (
                    <div
                      key={name}
                      className="flex flex-wrap items-baseline gap-x-2"
                    >
                      <dt className="text-sm font-medium text-foreground">
                        {name}
                      </dt>
                      <dd className="text-xs text-muted-foreground">{note}</dd>
                    </div>
                  ))}
                </dl>
              </CardContent>
            </Card>
          ))}
        </div>
      </section>

      <Separator />

      <footer className="flex flex-col gap-4 animate-rise">
        <div className="flex flex-wrap items-center gap-x-6 gap-y-3">
          <a
            href={REPO_URL}
            target="_blank"
            rel="noreferrer"
            className="group inline-flex items-center gap-1.5 text-sm font-medium text-foreground transition-colors duration-200 ease-soft hover:text-primary"
          >
            Source on GitHub
            <ExternalLink className="size-3.5 transition-transform duration-300 ease-spring group-hover:-translate-y-0.5 group-hover:translate-x-0.5" />
          </a>
          <a
            href={`${REPO_URL}/blob/main/LICENSE`}
            target="_blank"
            rel="noreferrer"
            className="inline-flex items-center gap-1.5 text-sm text-muted-foreground transition-colors duration-200 ease-soft hover:text-foreground"
          >
            <ShieldAlert className="size-3.5" />
            MIT licensed, no warranty
          </a>
        </div>
        <p className="text-xs leading-relaxed text-muted-foreground">
          Provided as is, without warranty of any kind. The author accepts no
          liability for damage, injury, or loss arising from its use, at any
          stage of its development. Use entirely at your own risk.
        </p>
      </footer>
    </main>
  );
}
