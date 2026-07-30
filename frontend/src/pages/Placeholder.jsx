export function PlaceholderPage({ title }) {
  return (
    <main className="mx-auto flex max-w-7xl flex-col items-center justify-center gap-2 px-4 py-24 text-center sm:px-6 lg:px-8">
      <h1 className="font-heading text-2xl font-semibold text-foreground">
        {title}
      </h1>
      <p className="text-sm text-muted-foreground">This page is coming soon.</p>
    </main>
  );
}
