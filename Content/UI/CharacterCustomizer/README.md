# Character Customizer UI Workspace

This folder is the project-owned staging area for the in-game/preround character customizer.

- `Images/` - hand-authored UI art, paper panels, button art, category icons.
- `Thumbnails/` - generated or captured item thumbnails for customization cards.
- `Materials/` - UI materials and dynamic material instances for hover/selection states.
- `Data/` - curated customization catalogs once the runtime registry moves out of filename scanning.

Current mesh sources remain in `/Game/Creative_Characters/Skeleton_Meshes` and are organized with local editor collections named `VNH_Customizer_*`.
Do not move marketplace source assets here until the customizer is gameplay-tested and redirectors can be fixed in one pass.
