import Link from "next/link";

// The cppreference-style trail: gti · std · filesystem · path. The last
// entry is the current page and is not a link.
export default function Breadcrumbs({
  trail,
}: {
  trail: { label: string; href?: string }[];
}) {
  return (
    <p className="breadcrumbs">
      <Link href="/">gti</Link>
      {trail.map((crumb) => (
        <span key={crumb.label}>
          {" · "}
          {crumb.href ? (
            <Link href={crumb.href}>{crumb.label}</Link>
          ) : (
            <span>{crumb.label}</span>
          )}
        </span>
      ))}
    </p>
  );
}
