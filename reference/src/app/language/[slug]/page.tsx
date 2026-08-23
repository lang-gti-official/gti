import type { Metadata } from "next";
import { notFound } from "next/navigation";
import language from "@/generated/language.json";
import { renderMarkdown } from "@/lib/markdown";
import Navbar from "@/components/Navbar";

// Every section page renders the normative markdown from docs/language/ as
// captured at build time — the text is quoted, never paraphrased, so the
// site cannot drift from the specification it presents. The markdown's own
// top-level heading becomes the page title.

export function generateStaticParams() {
  return language.sections.map((section) => ({ slug: section.slug }));
}

export async function generateMetadata({
  params,
}: {
  params: Promise<{ slug: string }>;
}): Promise<Metadata> {
  const { slug } = await params;
  const section = language.sections.find((s) => s.slug === slug);
  return { title: section ? section.title.replace(/^\d+\.\s*/, "") : "Language" };
}

export default async function LanguageSection({
  params,
}: {
  params: Promise<{ slug: string }>;
}) {
  const { slug } = await params;
  const section = language.sections.find((s) => s.slug === slug);
  if (!section) notFound();

  // Drop the document's own H1 — the page heading carries it.
  const body = section.markdown.replace(/^# .+\n/, "");
  const html = await renderMarkdown(body);

  return (
    <>
      <h1 id="firstHeading">{section.title.replace(/^\d+\.\s*/, "")}</h1>
      <div id="mw-content-text" className="spec-section">
        <Navbar />
        <div dangerouslySetInnerHTML={{ __html: html }} />
      </div>
    </>
  );
}
