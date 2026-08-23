import Link from "next/link";
import language from "@/generated/language.json";
import units from "@/generated/units.json";

// The t-navbar strip at the top of every content page, as on cppreference:
// a row of area links, each opening a menu on hover. Menus are plain tables
// in the t-nv style and are filled from generated data so they can never
// list a page that does not exist.

function Menu({ rows }: { rows: { label: string; href: string }[] }) {
  return (
    <div className="t-navbar-menu">
      <table className="t-nv-begin">
        <tbody>
          {rows.map((row) => (
            <tr className="t-nv" key={row.label}>
              <td>
                <Link href={row.href}>{row.label}</Link>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

export default function Navbar() {
  return (
    <div className="t-navbar">
      <div className="t-navbar-head">
        <Link href="/">GTI</Link>
        <Menu
          rows={[
            { label: "Language", href: "/language/" },
            { label: "Standard library", href: "/std/" },
            { label: "Standard library headers", href: "/std/" },
            { label: "Diagnostics", href: "/diagnostics/" },
            { label: "About this reference", href: "/about/" },
          ]}
        />
      </div>
      <div className="t-navbar-head">
        <Link href="/language/">Language</Link>
        <Menu
          rows={language.sections.map((section) => ({
            label: section.title.replace(/^\d+\.\s*/, ""),
            href: `/language/${section.slug}/`,
          }))}
        />
      </div>
      <div className="t-navbar-head">
        <Link href="/std/">Standard library headers</Link>
        <Menu
          rows={units.units.map((unit) => ({
            label: `<std/${unit.name}>`,
            href: `/std/${unit.name}/`,
          }))}
        />
      </div>
      <div className="t-navbar-head">
        <Link href="/diagnostics/">Diagnostics</Link>
      </div>
    </div>
  );
}
