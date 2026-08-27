import type { Metadata } from "next";
import Link from "next/link";
import { notFound } from "next/navigation";

import { ApiReference } from "@/components/ApiReference";
import { ExampleReference } from "@/components/ExampleReference";
import { referencePages } from "@/generated/reference-pages";

export const dynamicParams = false;

export function generateStaticParams() {
  return referencePages.map((page) => ({ slug: page.route.split("/") }));
}

export async function generateMetadata({
  params,
}: {
  params: Promise<{ slug: string[] }>;
}): Promise<Metadata> {
  const { slug } = await params;
  const route = slug.join("/");
  const page = referencePages.find((candidate) => candidate.route === route);
  if (!page) {
    return {};
  }
  return {
    title: page.metadata.title,
    description: page.metadata.description,
    openGraph: {
      title: `${page.metadata.title} — GTI reference`,
      description: page.metadata.description,
    },
    twitter: {
      card: "summary",
      title: `${page.metadata.title} — GTI reference`,
      description: page.metadata.description,
    },
  };
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
  const crumbs = slug.map((segment, index) => {
    const crumbRoute = slug.slice(0, index + 1).join("/");
    const crumbPage = referencePages.find(
      (candidate) => candidate.route === crumbRoute,
    );
    return {
      route: crumbRoute,
      label: crumbPage?.metadata.title ?? segment,
    };
  });
  return (
    <article className="reference-page">
      <nav className="breadcrumbs" aria-label="Breadcrumb">
        <ol>
          <li>
            <Link href="/">Reference</Link>
          </li>
          {crumbs.map((crumb, index) => (
            <li key={crumb.route}>
              {index === crumbs.length - 1 ? (
                <span aria-current="page">{crumb.label}</span>
              ) : (
                <Link href={`/${crumb.route}/`}>{crumb.label}</Link>
              )}
            </li>
          ))}
        </ol>
      </nav>
      <header className="page-heading">
        <p className="eyebrow">{page.metadata.kind.replaceAll("-", " ")}</p>
        <h1>{page.metadata.title}</h1>
        <p className="summary">{page.metadata.description}</p>
      </header>
      <ApiReference declarations={page.apiDeclarations} />
      <div className="reference-prose">
        <Content />
      </div>
      <ExampleReference examples={page.examples} />
    </article>
  );
}
