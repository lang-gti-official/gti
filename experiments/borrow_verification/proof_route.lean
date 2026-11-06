import Std

/- Phase-two feasibility prototype, NOT the phase-three soundness theorem.
   These definitions are not yet connected to the Python or GTI implementations.
   No `sorry`, additional axioms, or externally trusted decision procedure. -/
namespace BorrowLab

structure Place where
  owner : Nat
  field : Option Nat

def overlaps (a b : Place) : Prop :=
  a.owner = b.owner ∧
    (a.field = none ∨ b.field = none ∨ a.field = b.field)

theorem overlap_symmetric (a b : Place) (h : overlaps a b) : overlaps b a := by
  exact ⟨h.1.symm,
    h.2.elim (fun left => Or.inr (Or.inl left))
      (fun rest => rest.elim Or.inl (fun same => Or.inr (Or.inr same.symm)))⟩

structure Availability where
  live : Place → Prop
  initialized : Place → Prop

def join (a b : Availability) : Availability :=
  ⟨fun p => a.live p ∧ b.live p, fun p => a.initialized p ∧ b.initialized p⟩

def readable (s : Availability) (p : Place) : Prop := s.live p ∧ s.initialized p

theorem joined_read_is_safe_on_both_edges (a b : Availability) (p : Place)
    (h : readable (join a b) p) : readable a p ∧ readable b p := by
  exact ⟨⟨h.1.1, h.2.1⟩, ⟨h.1.2, h.2.2⟩⟩

theorem missing_initialization_prevents_read (s : Availability) (p : Place)
    (missing : ¬ s.initialized p) : ¬ readable s p := by
  intro h
  exact missing h.2

inductive Permission where
  | shared
  | exclusive

structure Loan where
  place : Place
  permission : Permission

def compatible (a b : Permission) : Prop :=
  a = Permission.shared ∧ b = Permission.shared

def grant (active : List Loan) (newLoan : Loan) : Prop :=
  ∀ prior, prior ∈ active → overlaps newLoan.place prior.place →
    compatible newLoan.permission prior.permission

theorem exclusive_overlap_prevents_grant (active : List Loan) (prior next : Loan)
    (present : prior ∈ active) (same : overlaps next.place prior.place)
    (exclusive : prior.permission = Permission.exclusive) : ¬ grant active next := by
  intro permitted
  have shared := (permitted prior present same).2
  rw [exclusive] at shared
  cases shared

#print axioms overlap_symmetric
#print axioms joined_read_is_safe_on_both_edges
#print axioms missing_initialization_prevents_read
#print axioms exclusive_overlap_prevents_grant

end BorrowLab
