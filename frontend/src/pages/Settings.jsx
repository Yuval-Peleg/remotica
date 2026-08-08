import { useEffect, useRef, useState } from "react";
import {
  Check,
  PencilLine,
  Search,
  Sparkles,
  TriangleAlert,
} from "lucide-react";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { Input } from "@/components/ui/input";
import { Button } from "@/components/ui/button";
import { Alert, AlertDescription } from "@/components/ui/alert";
import { Separator } from "@/components/ui/separator";
import { Textarea } from "@/components/ui/textarea";
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from "@/components/ui/dialog";
import {
  Select,
  SelectContent,
  SelectGroup,
  SelectItem,
  SelectLabel,
  SelectSeparator,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { ConnectionStatus } from "@/components/dashboard/ConnectionStatus";
import { usePrinterState } from "@/hooks/use-printer-state";
import { api } from "@/lib/api";
import { validateGcode } from "@/lib/gcode-validate";

const FIELDS = [
  { key: "bedWidthMm", label: "Bed width", unit: "mm" },
  { key: "bedDepthMm", label: "Bed depth", unit: "mm" },
  { key: "maxZMm", label: "Max Z height", unit: "mm" },
  { key: "minExtrudeTempC", label: "Min extrude temp", unit: "°C" },
  { key: "maxHotendTempC", label: "Max hotend temp", unit: "°C" },
  { key: "maxBedTempC", label: "Max bed temp", unit: "°C" },
];

const MANUAL_ID = "__manual__";

function formatNumber(value) {
  return Number(value).toString();
}

function buildVolume(specs) {
  return `${formatNumber(specs.bedWidthMm)} × ${formatNumber(specs.bedDepthMm)} × ${formatNumber(specs.maxZMm)} mm`;
}

function manualFormFrom(specs, name) {
  const form = { name: name ?? "" };
  for (const { key } of FIELDS) {
    form[key] = specs ? String(specs[key]) : "";
  }
  return form;
}

function SpecList({ specs }) {
  return (
    <dl className="grid grid-cols-2 gap-x-6 gap-y-3 sm:grid-cols-3">
      {FIELDS.map(({ key, label, unit }) => (
        <div key={key} className="flex flex-col gap-0.5">
          <dt className="text-xs text-muted-foreground">{label}</dt>
          <dd className="text-sm font-medium tabular-nums text-foreground">
            {formatNumber(specs[key])}
            <span className="ml-0.5 text-muted-foreground">{unit}</span>
          </dd>
        </div>
      ))}
    </dl>
  );
}

export function Settings() {
  const { connected, connectionType } = usePrinterState();

  const [profile, setProfile] = useState(null);
  const [printers, setPrinters] = useState([]);
  const [suggestion, setSuggestion] = useState(null);
  const [selectedId, setSelectedId] = useState("");
  const [manualForm, setManualForm] = useState(null);
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);

  // Start/end G-code. Loaded and saved independently of the profile —
  // see backend/src/gcode_snippets.h for why they're kept apart.
  const [snippets, setSnippets] = useState(null);
  const [snippetsSaving, setSnippetsSaving] = useState(false);
  const [snippetsSaved, setSnippetsSaved] = useState(false);
  const [snippetsError, setSnippetsError] = useState(null);
  // The profile change being held back by the printer-change dialog.
  const [pendingProfile, setPendingProfile] = useState(null);
  const [error, setError] = useState(null);

  useEffect(() => {
    api
      .getProfile()
      .then(setProfile)
      .catch(() => setError("Couldn't load the printer profile."));
    api
      .getPrinterDatabase()
      .then(setPrinters)
      .catch(() => {});
  }, []);

  // Re-fetched on every connect/disconnect, since the suggestion is derived
  // from a firmware reply that only exists while a printer is attached.
  useEffect(() => {
    api
      .getPrinterSuggestion()
      .then(setSuggestion)
      .catch(() => {});
  }, [connected]);

  // Picks the dropdown's starting value once the data it depends on has
  // arrived. Stops as soon as the user picks anything themselves, so a
  // suggestion landing late (printer connects while this page is open)
  // can't overwrite a choice they already made.
  const userPickedRef = useRef(false);
  useEffect(() => {
    if (userPickedRef.current || !profile || printers.length === 0) return;

    // "auto" as well as "default": an auto-detected profile is only a
    // proposal until someone confirms it, so the suggestion is still what
    // should be preselected — that's what makes confirming one click.
    if (profile.source !== "manual") {
      if (suggestion?.match) setSelectedId(suggestion.match.id);
      return;
    }

    const known = printers.find((p) => p.name === profile.printerName);
    if (known) {
      setSelectedId(known.id);
    } else {
      setSelectedId(MANUAL_ID);
      setManualForm(manualFormFrom(profile, profile.printerName));
    }
  }, [profile, printers, suggestion]);

  const selectedPreset = printers.find((p) => p.id === selectedId) ?? null;
  const isManual = selectedId === MANUAL_ID;

  // This panel reads everything out of the one /api/printer-suggestion
  // snapshot rather than mixing in the live WebSocket state: the match, the
  // firmware line it came from, and whether a printer was attached when it
  // was taken all have to describe the same moment, or the page can end up
  // claiming there's no printer while already holding a match for one. The
  // connection badge above still shows live state, and the snapshot is
  // refetched whenever that flips.
  const match = suggestion?.match ?? null;
  const printerAttached = suggestion?.connected ?? false;
  const firmwareInfo = suggestion?.firmwareInfo || "";

  // Names which of the four states the panel below is showing, purely so
  // it can be used as a React key: plugging a printer in mid-visit should
  // visibly replace the panel, not mutate it in place while nobody's
  // looking at that corner of the screen.
  const suggestionView =
    suggestion === null
      ? "checking"
      : !printerAttached
        ? "absent"
        : match
          ? `match:${match.id}`
          : "unknown";

  const handleSelect = (id) => {
    userPickedRef.current = true;
    setError(null);
    if (id === MANUAL_ID && manualForm === null) {
      // Carry the currently shown numbers over as a starting point, so
      // "manual" means "tweak these" rather than "start from an empty form".
      setManualForm(manualFormFrom(selectedPreset ?? profile, ""));
    }
    setSelectedId(id);
  };

  const updateManualField = (key, value) => {
    setManualForm((prev) => ({ ...prev, [key]: value }));
  };

  useEffect(() => {
    let cancelled = false;
    api
      .getSnippets()
      .then((data) => !cancelled && setSnippets(data))
      .catch(
        () =>
          !cancelled &&
          setSnippets({ startGcode: "", endGcode: "", writtenFor: "" })
      );
    return () => {
      cancelled = true;
    };
  }, []);

  // maxZMm comes from the confirmed profile, so "Z260 is above this
  // printer's maximum" can name the real number rather than a guess.
  const startWarnings = validateGcode(snippets?.startGcode ?? "", {
    position: "start",
    maxZMm: profile?.maxZMm,
  });
  const endWarnings = validateGcode(snippets?.endGcode ?? "", {
    position: "end",
    maxZMm: profile?.maxZMm,
  });
  const hasSnippets = Boolean(
    snippets?.startGcode?.trim() || snippets?.endGcode?.trim()
  );

  const saveSnippets = async (next) => {
    setSnippetsSaving(true);
    setSnippetsError(null);
    try {
      setSnippets(await api.setSnippets(next));
      setSnippetsSaved(true);
      setTimeout(() => setSnippetsSaved(false), 2000);
    } catch (err) {
      setSnippetsError(err.message || "Couldn't save the G-code.");
    } finally {
      setSnippetsSaving(false);
    }
  };

  // Applies a profile payload for real. Split out from handleSave so the
  // printer-change dialog can call it after the user has acknowledged
  // that their snippets were written for a different machine.
  const applyProfile = async (payload) => {
    setSaving(true);
    setError(null);
    try {
      setProfile(await api.setProfile(payload));
      setSaved(true);
      setTimeout(() => setSaved(false), 2000);
      // Snippets follow the printer they were checked against, so the
      // warning doesn't fire again for a change already acknowledged.
      if (snippets && snippets.writtenFor !== payload.printerName) {
        await saveSnippets({ ...snippets, writtenFor: payload.printerName });
      }
    } catch {
      setError("Couldn't save the profile — is the backend running?");
    } finally {
      setSaving(false);
    }
  };

  const handleSave = async () => {
    let payload;
    if (isManual) {
      const numbers = {};
      for (const { key, label } of FIELDS) {
        const value = Number(manualForm[key]);
        if (!Number.isFinite(value) || value <= 0) {
          setError(`${label} needs to be a number greater than zero.`);
          return;
        }
        numbers[key] = value;
      }
      payload = { ...numbers, printerName: manualForm.name.trim() };
    } else if (selectedPreset) {
      const numbers = Object.fromEntries(
        FIELDS.map(({ key }) => [key, Number(selectedPreset[key])])
      );
      payload = { ...numbers, printerName: selectedPreset.name };
    } else {
      return;
    }

    // Blocking, not advisory: start G-code that homes or heats correctly
    // for one machine can be actively wrong on another, and the moment
    // the user causes the change is the right moment to ask.
    if (
      hasSnippets &&
      snippets.writtenFor &&
      snippets.writtenFor !== payload.printerName
    ) {
      setPendingProfile(payload);
      return;
    }

    await applyProfile(payload);
  };

  const inUseLabel = !profile
    ? null
    : profile.source !== "manual"
      ? "Nothing confirmed yet"
      : profile.printerName || "Custom profile";

  return (
    <main className="mx-auto flex max-w-3xl flex-col gap-6 px-4 py-10 sm:px-6 lg:px-8">
      <div>
        <h1 className="font-heading text-2xl font-semibold text-foreground">
          Settings
        </h1>
        <p className="text-sm text-muted-foreground">
          Tell Remotica what printer it&apos;s driving, so it knows how far the
          bed reaches and how hot it&apos;s allowed to get.
        </p>
      </div>

      <Card>
        <CardHeader>
          <CardTitle className="text-base">Suggested printer</CardTitle>
        </CardHeader>
        <CardContent className="flex flex-col gap-4">
          <ConnectionStatus
            connected={connected}
            connectionType={connectionType}
          />

          <div
            key={suggestionView}
            className="flex animate-fade flex-col gap-4"
          >
            {suggestion === null ? (
              // Distinct from "nothing is connected": until this resolves we
              // genuinely don't know yet, and on a phone across the LAN that
              // gap is long enough to read.
              <p className="text-sm text-muted-foreground">
                Checking what&apos;s connected...
              </p>
            ) : !printerAttached ? (
              <p className="text-sm text-muted-foreground">
                No printer connected. Plug one in over USB and Remotica will ask
                it what it is.
              </p>
            ) : match ? (
              <>
                <div className="flex items-start gap-3">
                  <Sparkles className="mt-0.5 size-5 shrink-0 text-primary" />
                  <div className="flex flex-col gap-0.5">
                    <span className="font-heading text-lg leading-tight font-semibold text-foreground">
                      {match.name}
                    </span>
                    <span className="text-xs text-muted-foreground tabular-nums">
                      {buildVolume(match)} build volume · up to{" "}
                      {formatNumber(match.maxHotendTempC)}°C hotend,{" "}
                      {formatNumber(match.maxBedTempC)}°C bed
                    </span>
                  </div>
                </div>

                {firmwareInfo && (
                  <div className="rounded-md border border-border bg-background/60 p-2.5">
                    <p className="text-[0.65rem] font-medium tracking-wide text-muted-foreground uppercase">
                      What the printer reported
                    </p>
                    <p className="mt-1 font-mono text-xs break-all text-foreground">
                      {firmwareInfo}
                    </p>
                  </div>
                )}

                <Separator />
                <p className="text-sm text-muted-foreground">
                  Remotica thinks this is a{" "}
                  <span className="font-medium text-foreground">
                    {match.name}
                  </span>
                  , going by the model name in that reply. Check it yourself
                  before you print — the wrong match means the wrong bed size
                  and temperature limits.
                </p>
              </>
            ) : (
              <>
                <div className="flex items-start gap-3">
                  <Search className="mt-0.5 size-5 shrink-0 text-muted-foreground" />
                  <div className="flex flex-col gap-0.5">
                    <span className="font-heading text-lg leading-tight font-semibold text-foreground">
                      Not recognised
                    </span>
                    <span className="text-xs text-muted-foreground">
                      This printer didn&apos;t report a model name Remotica
                      knows.
                    </span>
                  </div>
                </div>

                {firmwareInfo && (
                  <div className="rounded-md border border-border bg-background/60 p-2.5">
                    <p className="text-[0.65rem] font-medium tracking-wide text-muted-foreground uppercase">
                      What the printer reported
                    </p>
                    <p className="mt-1 font-mono text-xs break-all text-foreground">
                      {firmwareInfo}
                    </p>
                  </div>
                )}

                <Separator />
                <p className="text-sm text-muted-foreground">
                  Most printers don&apos;t announce their model over USB, so
                  this is normal. Pick yours below, or enter its specs by hand.
                </p>
              </>
            )}
          </div>
        </CardContent>
      </Card>

      <Card>
        <CardHeader>
          <CardTitle className="text-base">Choose your printer</CardTitle>
        </CardHeader>
        <CardContent className="flex flex-col gap-4">
          {profile && profile.source !== "manual" && (
            <Alert className="animate-pop">
              <AlertDescription>
                {profile.source === "auto" && profile.printerName
                  ? `Remotica thinks this is a ${profile.printerName}, but nothing is confirmed yet. Jogging, homing, and starting a print stay blocked until you save a printer here.`
                  : "No printer chosen yet. Jogging, homing, and starting a print stay blocked until you save one here."}
              </AlertDescription>
            </Alert>
          )}

          <div className="flex flex-col gap-1.5">
            <label className="text-xs font-medium text-muted-foreground">
              Printer
            </label>
            <Select value={selectedId} onValueChange={handleSelect}>
              <SelectTrigger className="w-full">
                <SelectValue placeholder="Pick a printer..." />
              </SelectTrigger>
              <SelectContent>
                {match && (
                  <SelectGroup>
                    <SelectLabel>Suggested</SelectLabel>
                    <SelectItem value={match.id}>{match.name}</SelectItem>
                  </SelectGroup>
                )}
                {match && <SelectSeparator />}
                <SelectGroup>
                  {match && <SelectLabel>Everything else</SelectLabel>}
                  {printers
                    .filter((p) => p.id !== match?.id)
                    .map((printer) => (
                      <SelectItem key={printer.id} value={printer.id}>
                        {printer.name}
                      </SelectItem>
                    ))}
                </SelectGroup>
                <SelectSeparator />
                <SelectItem value={MANUAL_ID}>
                  <PencilLine className="size-3.5" />
                  Enter specs by hand
                </SelectItem>
              </SelectContent>
            </Select>
            {inUseLabel && (
              <p className="text-xs text-muted-foreground">
                In use right now:{" "}
                <span className="text-foreground">{inUseLabel}</span>
              </p>
            )}
          </div>

          {isManual && manualForm && (
            <div className="flex animate-rise flex-col gap-4">
              <div className="flex flex-col gap-1.5">
                <label
                  htmlFor="printerName"
                  className="text-xs font-medium text-muted-foreground"
                >
                  Name this printer
                </label>
                <Input
                  id="printerName"
                  value={manualForm.name}
                  placeholder="e.g. Ender 3 with the taller frame"
                  onChange={(e) => updateManualField("name", e.target.value)}
                />
              </div>

              <div className="grid grid-cols-1 gap-4 sm:grid-cols-2">
                {FIELDS.map(({ key, label, unit }) => (
                  <div key={key} className="flex flex-col gap-1.5">
                    <label
                      htmlFor={key}
                      className="text-xs font-medium text-muted-foreground"
                    >
                      {label} ({unit})
                    </label>
                    <Input
                      id={key}
                      type="number"
                      value={manualForm[key]}
                      onChange={(e) => updateManualField(key, e.target.value)}
                    />
                  </div>
                ))}
              </div>
            </div>
          )}

          {/* Keyed on the selection so switching between two presets
              replays the entrance — without it React updates the same
              <dl> in place and six numbers silently change value, which
              is easy to miss when they're the whole point of the panel. */}
          {selectedPreset && (
            <div key={selectedId} className="animate-rise">
              <SpecList specs={selectedPreset} />
            </div>
          )}

          {error && (
            <Alert variant="destructive" className="animate-pop">
              <AlertDescription>{error}</AlertDescription>
            </Alert>
          )}

          {(selectedPreset || (isManual && manualForm)) && (
            <div className="flex animate-rise items-center gap-3">
              <Button onClick={handleSave} disabled={saving}>
                {saving ? "Saving..." : "Use this printer"}
              </Button>
              {saved && (
                <span className="flex animate-pop items-center gap-1 text-sm text-primary">
                  <Check className="size-4" />
                  Saved
                </span>
              )}
            </div>
          )}
        </CardContent>
      </Card>

      <Card>
        <CardHeader>
          <CardTitle className="text-base">Start &amp; end G-code</CardTitle>
        </CardHeader>
        <CardContent className="flex flex-col gap-5">
          <p className="text-sm text-muted-foreground">
            Runs before and after every print, around the sliced file&apos;s own
            start and end blocks. Test it with the printer in front of you.
          </p>

          {snippets?.writtenFor &&
            profile?.printerName &&
            snippets.writtenFor !== profile.printerName && (
              <Alert className="animate-pop border-amber-500/40 bg-amber-500/10">
                <TriangleAlert className="size-4 text-amber-400" />
                <AlertDescription className="text-amber-200">
                  This G-code was written for {snippets.writtenFor}, not{" "}
                  {profile.printerName}. Check it before printing.
                </AlertDescription>
              </Alert>
            )}

          <SnippetField
            label="Before every print"
            value={snippets?.startGcode ?? ""}
            warnings={startWarnings}
            disabled={snippets === null}
            onChange={(value) =>
              setSnippets((prev) => ({ ...prev, startGcode: value }))
            }
          />

          <SnippetField
            label="After every print"
            value={snippets?.endGcode ?? ""}
            warnings={endWarnings}
            disabled={snippets === null}
            onChange={(value) =>
              setSnippets((prev) => ({ ...prev, endGcode: value }))
            }
          />

          {snippetsError && (
            <Alert variant="destructive" className="animate-pop">
              <AlertDescription>{snippetsError}</AlertDescription>
            </Alert>
          )}

          <div className="flex items-center gap-3">
            <Button
              variant="secondary"
              disabled={snippets === null || snippetsSaving}
              onClick={() =>
                saveSnippets({
                  ...snippets,
                  writtenFor: profile?.printerName ?? "",
                })
              }
            >
              {snippetsSaving ? "Saving..." : "Save G-code"}
            </Button>
            {snippetsSaved && (
              <span className="flex animate-pop items-center gap-1 text-sm text-primary">
                <Check className="size-4" />
                Saved
              </span>
            )}
          </div>
        </CardContent>
      </Card>

      <Dialog
        open={pendingProfile !== null}
        onOpenChange={(open) => !open && setPendingProfile(null)}
      >
        <DialogContent
          className="sm:max-w-md"
          onInteractOutside={(e) => e.preventDefault()}
        >
          <DialogHeader>
            <DialogTitle>
              Your G-code was written for {snippets?.writtenFor}
            </DialogTitle>
            <DialogDescription>
              You&apos;re switching to {pendingProfile?.printerName}. Start
              G-code that homes or heats correctly for one machine can be wrong
              on another — it runs before every print.
            </DialogDescription>
          </DialogHeader>
          <DialogFooter className="gap-2 sm:gap-2">
            <Button variant="secondary" onClick={() => setPendingProfile(null)}>
              Review G-code
            </Button>
            <Button
              onClick={() => {
                const payload = pendingProfile;
                setPendingProfile(null);
                applyProfile(payload);
              }}
            >
              I&apos;ve checked — switch anyway
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </main>
  );
}

// One labelled G-code box plus its warnings. Warnings are advisory only —
// nothing here can prevent saving; see lib/gcode-validate.js.
function SnippetField({ label, value, warnings, disabled, onChange }) {
  return (
    <div className="flex flex-col gap-1.5">
      <div className="flex items-baseline justify-between gap-3">
        <label className="text-xs font-medium text-muted-foreground">
          {label}
        </label>
        {value.length > 1500 && (
          <span className="text-xs tabular-nums text-muted-foreground">
            {value.length} / 2047
          </span>
        )}
      </div>
      <Textarea
        value={value}
        disabled={disabled}
        onChange={(e) => onChange(e.target.value)}
        rows={5}
        spellCheck={false}
        placeholder="; one command per line"
        className="font-mono text-xs"
      />
      {warnings.length > 0 && (
        <ul className="flex flex-col gap-1 pt-0.5">
          {warnings.map((warning, index) => (
            <li
              key={`${warning.line}-${index}`}
              className="flex items-start gap-1.5 text-xs text-amber-400"
            >
              <TriangleAlert className="mt-0.5 size-3 shrink-0" />
              <span>
                {warning.line != null && (
                  <span className="font-medium">Line {warning.line}: </span>
                )}
                {warning.message}
              </span>
            </li>
          ))}
        </ul>
      )}
    </div>
  );
}
