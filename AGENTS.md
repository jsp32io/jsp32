# Project preferences

- The root `README.md` is for project users. Describe current features, usage, and project structure; omit changelogs, migration history, and development logs.
- Use English in `README.md` and Chinese in `README_zh.md`. Keep both versions consistent and include language-switch links.
- Prefer compact UI layouts with polished details: reduce unnecessary whitespace while keeping a clear visual hierarchy and easy-to-use controls.
- Place page status messages to the right of the title. Use green for normal status and a prominent color for errors.
- Prefer vendoring third-party firmware dependencies into `components/` so normal builds do not require component registry downloads.
  Preserve upstream licenses and record the version, source, and commit in `SOURCE.md`; update these records when upgrading dependencies.
  cJSON, mDNS, and MQTT use local components. Continue this approach.
