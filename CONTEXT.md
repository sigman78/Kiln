# Kiln assets

Kiln prepares assets for storage and loads their cooked representation for a host renderer.

## Language

**Cooked asset**:
An asset prepared for runtime consumption, including the metadata and payload needed to use it.

**Cooked-asset read path**:
The work required to load an existing cooked asset and make it available to the host renderer,
including reading, validation, payload decoding, and upload. It excludes producing or saving the
cooked asset.

**Load attempt**:
One attempt to obtain a particular version of an asset. A reload is a new attempt, while the
previous successful version can remain available.

**Cook on miss**:
Producing a cooked asset because the requested cooked representation is absent from the store.
It is separate from loading an existing cooked asset.

**Readiness set**:
A host-selected collection of required assets whose combined availability determines whether a
renderable object or material may be used. The same asset may be required by several sets.

**Sealed readiness set**:
A readiness set whose required membership has been fully declared for its current revision.
It can report readiness once every required asset is usable.
