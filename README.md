# pgColumnar

A column-oriented storage extension for PostgreSQL, implemented as a table
access method. pgColumnar is an independent implementation. It is not derived
from the source of any other columnar project. It is built from a functional
specification of the on-disk format and SQL interface, recorded in
[design/FORMAT_AND_INTERFACE_SPEC.md](design/FORMAT_AND_INTERFACE_SPEC.md).

Licensed under the MIT License. See [LICENSE](LICENSE).

Status: early construction. See [design/REWRITE_PLAN.md](design/REWRITE_PLAN.md)
for the delivery phases and task list, and [PROVENANCE.md](PROVENANCE.md) for how
the implementation stays independent.
