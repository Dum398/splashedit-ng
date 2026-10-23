// The Strings table: not psxsplash's, the exporter writes it (core/strings.cpp)
// for scenes with localised strings (strings.json, View > Strings). Hand-kept,
// unlike lua_api.inc. A scene carries only the keys it names: keyed UI texts,
// Strings.Get("literal") keys, and keys under the scene's String prefixes.
{"Strings", "", ApiKind::Table, "Strings", "Localised strings of this scene (the export generates it from strings.json when the scene uses them)."},
{"Strings", "Get", ApiKind::Function, "Strings.Get(key) -> string", "The key's text in the active language, the default language's when that is empty, the key itself when the scene does not carry it. Write the key as a string literal (or add its prefix to the scene's String prefixes) so the export carries it."},
{"Strings", "Language", ApiKind::Function, "Strings.Language() -> string", "The id of the active language (\"en\")."},
{"Strings", "Languages", ApiKind::Function, "Strings.Languages() -> {string, ...}", "The language ids, the default first."},
{"Strings", "SetLanguage", ApiKind::Function, "Strings.SetLanguage(id|index) -> boolean", "Makes a language active, by id or 1-based index; re-applies the scene's keyed UI texts; it stays across scene loads (a Persist slot). false for an unknown language. Does not touch the memory card."},
{"Strings", "Save", ApiKind::Function, "Strings.Save() -> boolean [, error]", "Saves the active language to the memory card in port 1 (one block; the game waits about 2 s). With Remember language on memory card on, the next boot starts in it."},
