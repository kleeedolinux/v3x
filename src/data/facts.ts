/**
 * Facts the landing and the guide print. Every number and default comes from the V3X repo:
 * README.md ("What we built with V3X", "For players"), docs/install.md ("Which plugins are on"),
 * and docs/plugins.md. Update them here when those change.
 * Which plugins are on after install is NOT written here: it comes from installer/profile.txt,
 * turned into ./profile.json by scripts/sync-docs.ts.
 */
import profile from "./profile.json";

export const REPO_URL = "https://github.com/kleeedolinux/v3x";
export const RELEASES_URL = "https://github.com/kleeedolinux/v3x/releases";
export const GAME_VERSION = "1.15.1";
/** docs/install.md "The easy way: one line". */
export const INSTALL_CMD = "irm https://v3extender.pages.dev/install.ps1 | iex";
export const BRAND = { short: "V3X", full: "Victoria 3 eXtender" };

/** README.md: "All numbers were measured on the author's PC (16 GB RAM, integrated GPU, Victoria 3 1.15.1)." */
export const MACHINE = { ram: "16 GB", gpu: "integrated", game: "1.15.1" };

export const measured = {
  meshLoad: { before: 37.5, after: 6.7, unit: "s", types: 2118, plugin: "lazy_mesh" },
  debugSave: { before: 156, after: 26, unit: "MB", plugin: "enhance" },
  fileLookups: { misses: "3.44 M", cost: "103.8 s", predicted: "94.4 %", mismatches: 0, mods: 23 },
  memReturned: { mib: "1,548 MiB", runs: 16, session: "1 h 44 m" },
};

/** "on" = installed and loaded, "off" = installed but off (turn on in main menu > V3X), "none" = not shipped. */
export type PluginState = "on" | "off" | "none";
const STATES = new Map(profile.plugins.map((p) => [p.name, p.state as PluginState]));

/** The state installer/profile.txt gives a plugin. Throws (fails the build) for a name it does not list. */
export function pluginState(id: string): PluginState {
  const s = STATES.get(id);
  if (!s) throw new Error(`facts.ts: plugin "${id}" is not in installer/profile.txt (src/data/profile.json)`);
  return s;
}
export const isOn = (id: string) => pluginState(id) === "on";
/** Plugins setup installs and switches on, in profile.txt order. */
export const onAfterInstall: string[] = profile.plugins.filter((p) => p.state === "on").map((p) => p.name);

type L = { en: string; pt: string };
export interface ShippedPlugin { id: string; on: boolean; what: L }

/** One-line plain descriptions from docs/plugins.md; on/off comes from installer/profile.txt. */
const described: { id: string; what: L }[] = [
  { id: "mod_menu", what: {
    en: "The <b>V3X</b> button on the main menu, with a checkbox for each part.",
    pt: "O botão <b>V3X</b> no menu principal, com uma caixa de seleção para cada parte." } },
  { id: "keep_history", what: {
    en: "Keeps the GDP, population and other graphs of a country you stop playing; AI countries are recorded as often as yours (every 28 days).",
    pt: "Mantém os gráficos de PIB, população e outros de um país que você deixa de jogar; países da IA são registrados com a mesma frequência que o seu (a cada 28 dias)." } },
  { id: "mem_tuner", what: {
    en: "Tunes the game's memory use, gives memory back after loads, keeps a 128 MB emergency reserve and explains an out-of-memory error in the log.",
    pt: "Ajusta o uso de memória do jogo, devolve memória depois dos carregamentos, guarda uma reserva de emergência de 128 MB e explica no log um erro de falta de memória." } },
  { id: "frame_pacer", what: {
    en: "The frame cap sleeps instead of running a core at 100 %; lower frame rate when paused, idle, in the background or minimised.",
    pt: "O limite de quadros dorme em vez de manter um núcleo a 100 %; menos quadros quando pausado, parado, em segundo plano ou minimizado." } },
  { id: "cpu_sched", what: {
    en: "On CPUs with performance and efficiency cores, keeps the game's main thread on the performance cores. Does nothing on other CPUs.",
    pt: "Em processadores com núcleos de desempenho e de eficiência, mantém a thread principal do jogo nos núcleos de desempenho. Não faz nada nos outros." } },
  { id: "crash_logger", what: {
    en: "Writes <code>crash.log</code> when something goes wrong: the file to send with a bug report.",
    pt: "Escreve o <code>crash.log</code> quando algo dá errado: o arquivo para enviar junto com um relato de erro." } },
  { id: "ironman_console", what: {
    en: "Lets the console work in ironman (multiplayer stays blocked). Leave it off if achievements matter to you.",
    pt: "Libera o console no modo ironman (o multijogador continua bloqueado). Deixe desligado se as conquistas importam para você." } },
  { id: "gfx_driver", what: {
    en: "Asks the graphics driver not to add its own threading. The effect on frame rate is not measured yet.",
    pt: "Pede ao driver de vídeo que não crie threads próprias. O efeito nos quadros por segundo ainda não foi medido." } },
  { id: "log_filter", what: {
    en: "Drops repeated, noisy lines from the engine's logs.",
    pt: "Remove linhas repetidas e ruidosas dos logs do motor." } },
  { id: "fast_load", what: {
    en: "Measures loading and tells the other parts when a load starts and ends.",
    pt: "Mede o carregamento e avisa as outras partes quando um carregamento começa e termina." } },
  { id: "content_packs", what: {
    en: "Lets files in <code>binaries\\v3x\\content_packs\\</code> replace game files without a mod. Needed by the V3X button.",
    pt: "Permite que arquivos em <code>binaries\\v3x\\content_packs\\</code> substituam arquivos do jogo sem um mod. Necessário para o botão V3X." } },
  { id: "lazy_mesh", what: {
    en: "Loads the 3D models your game uses at start and the rest on first use: 37.5 s down to 6.7 s in a measured run.",
    pt: "Carrega no início os modelos 3D que o seu jogo usa e o resto no primeiro uso: de 37,5 s para 6,7 s numa medição." } },
  { id: "loc_fallback", what: {
    en: "Fills text that a mod lacks in your language with the mod's English text.",
    pt: "Preenche com o texto em inglês do mod as linhas que faltam no seu idioma." } },
  { id: "enhance", what: {
    en: "In debug mode, saves in the normal compressed format: 156 MB down to 26 MB per autosave.",
    pt: "No modo debug, salva no formato comprimido normal: de 156 MB para 26 MB por autosave." } },
  { id: "console_plus", what: {
    en: "Tab completion in the console: country tags, states, laws.",
    pt: "Completar com Tab no console: tags de países, estados, leis." } },
  { id: "asset_stream", what: {
    en: "Forces the game's own texture streaming on (every quality preset turns it off).",
    pt: "Força o streaming de texturas do próprio jogo (todas as predefinições de qualidade o desligam)." } },
  { id: "sim_opt", what: {
    en: "Simulation diagnostics. Its optional throttle changes the simulation: back up saves first.",
    pt: "Diagnóstico da simulação. A opção de throttle altera a simulação: faça backup dos saves antes." } },
  { id: "doctor", what: {
    en: "A health check: type <code>doctor</code> in the console for a report on the game version, V3X's files, memory, disk space and the last crash. Read-only; it changes a setting only when you ask with <code>doctor fix</code>.",
    pt: "Um check-up: digite <code>doctor</code> no console para um relatório da versão do jogo, dos arquivos do V3X, da memória, do espaço em disco e do último travamento. Só lê; muda uma configuração apenas quando você pede com <code>doctor fix</code>." } },
  { id: "script_ext", what: {
    en: "Lets other parts ship game script, text and interface files. Hooks nothing by itself.",
    pt: "Permite que outras partes tragam arquivos de script, texto e interface do jogo. Sozinho não intercepta nada." } },
  { id: "updater", what: {
    en: "Automatic updates: checks for a new V3X release and installs it when you quit the game. Off for now, until the first release that supports it.",
    pt: "Atualização automática: procura uma versão nova do V3X e instala quando você fecha o jogo. Desligado por enquanto, até sair a primeira versão compatível." } },
];

/** The plugin table: every described plugin setup installs, with its state from installer/profile.txt. */
export const shipped: ShippedPlugin[] = described
  .filter((p) => pluginState(p.id) !== "none")
  .map((p) => ({ ...p, on: isOn(p.id) }));
