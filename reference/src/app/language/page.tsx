import type { Metadata } from "next";
import language from "@/generated/language.json";
import { Page, Descriptions } from "@/components/Cpp";

export const metadata: Metadata = { title: "GTI language" };

// cppreference.com/w/cpp/language: a description table of the language
// sections. Titles and status lines come from docs/language/ at build time.
export default function LanguageIndex() {
  return (
    <Page title="GTI language">
      <p>
        The language pages present the GTI specification — lexical structure,
        static semantics, ownership and lifetimes, execution, and target
        behaviour — rendered from the normative text in{" "}
        <code>docs/language/</code> exactly as it stands in the repository.
      </p>
      <h3>Sections</h3>
      <Descriptions
        groups={[
          {
            rows: language.sections.map((section) => ({
              name: section.title.replace(/^\d+\.\s*/, ""),
              href: `/language/${section.slug}/`,
              description: section.status
                ? `status: ${section.status}`
                : `docs/language/${section.file}`,
              kind: "specification section",
            })),
          },
        ]}
      />
    </Page>
  );
}
