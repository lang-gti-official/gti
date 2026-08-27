import Link from "next/link";

const referenceSections = [
  {
    title: "Language",
    href: "/language/",
    description: "Syntax, types, ownership, control flow, and interoperability.",
    links: [{ title: "Language overview", href: "/language/" }],
  },
  {
    title: "Standard library",
    href: "/std/",
    description:
      "Shipped headers, types, functions, and implementation status.",
    links: [
      { title: "Header index", href: "/std/" },
      { title: "std::array", href: "/std/array/array/" },
      { title: "std::string", href: "/std/string/string/" },
      { title: "std::vector", href: "/std/vector/vector/" },
    ],
  },
  {
    title: "Tooling",
    href: "/tooling/",
    description:
      "Compiler, project, native-binding, formatting, and editor workflows.",
    links: [
      { title: "Command-line interface", href: "/tooling/cli/" },
      { title: "Projects and gti.toml", href: "/tooling/projects/" },
      { title: "C header bindings", href: "/tooling/bindgen/" },
      { title: "Formatter", href: "/tooling/formatting/" },
      { title: "Language server", href: "/tooling/lsp/" },
    ],
  },
] as const;

export default function HomePage() {
  return (
    <article className="reference-page home-page">
      <header className="home-heading">
        <h1>GTI reference</h1>
        <p className="summary">
          Language syntax, standard-library APIs, compiler commands, project
          configuration, and editor tooling.
        </p>
      </header>
      <nav className="reference-index" aria-label="Reference contents">
        {referenceSections.map((section) => (
          <section key={section.href}>
            <header>
              <h2>
                <Link href={section.href}>{section.title}</Link>
              </h2>
              <p>{section.description}</p>
            </header>
            <ul>
              {section.links.map((link) => (
                <li key={link.href}>
                  <Link href={link.href}>{link.title}</Link>
                </li>
              ))}
            </ul>
          </section>
        ))}
      </nav>
    </article>
  );
}
