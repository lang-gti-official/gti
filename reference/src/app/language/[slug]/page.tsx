import type { Metadata } from "next";
import { notFound } from "next/navigation";
import language from "@/generated/language.json";
import { renderMarkdown } from "@/lib/markdown";
import Breadcrumbs from "@/components/Breadcrumbs";

// Every section page renders the normative markdown from docs/language/ as
// captured at build time — the text is quoted, never paraphrased, so the
// site cannot drift from the specification it presents.

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
  return { title: section ? section.title : "Language" };
}

export default async function LanguageSection({
  params,
}: {
  params: Promise<{ slug: string }>;
}) {
  const { slug } = await params;
  const section = language.sections.find((s) => s.slug === slug);
  if (!section) notFound();

  const html = await renderMarkdown(section.markdown);

  return (
    <article className="spec-section">
      <Breadcrumbs
        trail={[{ label: "language", href: "/language/" }, { label: slug }]}
      />
      <p className="muted" style={{ fontSize: "0.8125rem", margin: 0 }}>
        Normative source: <code>docs/language/{section.file}</code>
        {section.status ? (
          <>
            {" · "}
            <span className="badge">{section.status}</span>
          </>
        ) : null}
      </p>
      <div dangerouslySetInnerHTML={{ __html: html }} />
    </article>
  );
}
