import { useEffect, useState } from "react";
import { Check, Info } from "lucide-react";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { Input } from "@/components/ui/input";
import { Button } from "@/components/ui/button";
import { Alert, AlertDescription } from "@/components/ui/alert";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { ConnectionStatus } from "@/components/dashboard/ConnectionStatus";
import { usePrinterState } from "@/hooks/use-printer-state";
import { api } from "@/lib/api";

const FIELDS = [
  { key: "bedWidthMm", label: "Bed width", unit: "mm" },
  { key: "bedDepthMm", label: "Bed depth", unit: "mm" },
  { key: "maxZMm", label: "Max Z height", unit: "mm" },
  { key: "minExtrudeTempC", label: "Min extrude temp", unit: "°C" },
  { key: "maxHotendTempC", label: "Max hotend temp", unit: "°C" },
  { key: "maxBedTempC", label: "Max bed temp", unit: "°C" },
];

export function Settings() {
  const { connected, connectionType, firmwareInfo } = usePrinterState();

  const [form, setForm] = useState(null);
  const [printers, setPrinters] = useState([]);
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);
  const [error, setError] = useState(null);

  useEffect(() => {
    api
      .getProfile()
      .then(setForm)
      .catch(() => setError("Couldn't load the printer profile."));
    api
      .getPrinterDatabase()
      .then(setPrinters)
      .catch(() => {});
  }, []);

  const applyPreset = (id) => {
    const preset = printers.find((p) => p.id === id);
    if (!preset) return;
    // eslint-disable-next-line no-unused-vars
    const { id: _id, name: _name, ...profileFields } = preset;
    setForm((prev) => ({ ...prev, ...profileFields }));
  };

  const updateField = (key, value) => {
    setForm((prev) => ({ ...prev, [key]: value }));
  };

  const handleSave = async () => {
    setSaving(true);
    setError(null);
    try {
      const numericForm = Object.fromEntries(
        Object.entries(form).map(([key, value]) => [key, Number(value)])
      );
      const updated = await api.setProfile(numericForm);
      setForm(updated);
      setSaved(true);
      setTimeout(() => setSaved(false), 2000);
    } catch {
      setError("Couldn't save the profile — is the backend running?");
    } finally {
      setSaving(false);
    }
  };

  return (
    <main className="mx-auto flex max-w-3xl flex-col gap-6 px-4 py-10 sm:px-6 lg:px-8">
      <div>
        <h1 className="font-heading text-2xl font-semibold text-foreground">
          Settings
        </h1>
        <p className="text-sm text-muted-foreground">
          Your printer&apos;s physical specs — bed size, build height, and safe
          temperature limits. Nothing here is detected automatically; pick your
          printer below to quick-fill known values, or enter them by hand.
        </p>
      </div>

      <Card>
        <CardHeader>
          <CardTitle className="text-base">Connection</CardTitle>
        </CardHeader>
        <CardContent className="flex flex-col gap-3">
          <ConnectionStatus
            connected={connected}
            connectionType={connectionType}
          />

          <Alert>
            <Info className="size-4" />
            <AlertDescription>
              {firmwareInfo ? (
                <>
                  Printer replied to a firmware query with:{" "}
                  <span className="font-mono text-foreground">
                    {firmwareInfo}
                  </span>
                  . This is a hint, not a reliable identification — most
                  firmwares don&apos;t report a specific model name, so use it
                  to help you pick a printer below, not as an automatic answer.
                </>
              ) : (
                <>
                  No firmware info reported yet. Even when a printer does reply,
                  USB and Wi-Fi connections can&apos;t reliably identify which
                  specific model is attached — there&apos;s no standard way to
                  ask. Pick your printer below, or enter its specs manually.
                </>
              )}
            </AlertDescription>
          </Alert>
        </CardContent>
      </Card>

      <Card>
        <CardHeader>
          <CardTitle className="text-base">Printer profile</CardTitle>
        </CardHeader>
        <CardContent className="flex flex-col gap-4">
          {printers.length > 0 && (
            <div className="flex flex-col gap-1.5">
              <label className="text-xs font-medium text-muted-foreground">
                Quick-fill from a known printer
              </label>
              <Select onValueChange={applyPreset}>
                <SelectTrigger className="w-full">
                  <SelectValue placeholder="Select a printer model..." />
                </SelectTrigger>
                <SelectContent>
                  {printers.map((printer) => (
                    <SelectItem key={printer.id} value={printer.id}>
                      {printer.name}
                    </SelectItem>
                  ))}
                </SelectContent>
              </Select>
            </div>
          )}

          {form && (
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
                    value={form[key]}
                    onChange={(e) => updateField(key, e.target.value)}
                  />
                </div>
              ))}
            </div>
          )}

          {error && (
            <Alert variant="destructive">
              <AlertDescription>{error}</AlertDescription>
            </Alert>
          )}

          <div className="flex items-center gap-3">
            <Button onClick={handleSave} disabled={!form || saving}>
              {saving ? "Saving..." : "Save"}
            </Button>
            {saved && (
              <span className="flex items-center gap-1 text-sm text-primary">
                <Check className="size-4" />
                Saved
              </span>
            )}
          </div>
        </CardContent>
      </Card>
    </main>
  );
}
