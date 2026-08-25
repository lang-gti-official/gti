import { notFound } from "next/navigation";

import { referencePages } from "@/generated/reference-pages";

export const dynamicParams = false;

export function generateStaticParams() {
  return referencePages.map((page) => ({ slug: page.route.split("/") }));
}

export default async function ReferencePage({
  params,
}: {
  params: Promise<{ slug: string[] }>;
}) {
  const { slug } = await params;
  const route = slug.join("/");
  const page = referencePages.find((candidate) => candidate.route === route);
  if (!page) {
    notFound();
  }
  const Content = page.Content;
  return (
    <article className="reference-page">
      <p className="eyebrow">{page.metadata.kind.replaceAll("-", " ")}</p>
      <h1>{page.metadata.title}</h1>
      <p className="summary">{page.metadata.description}</p>
      <Content />
    </article>
  );
}
