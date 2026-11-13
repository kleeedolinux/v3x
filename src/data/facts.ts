/**
 * Facts the landing and the guide print. Every number and default comes from the V3X repo:
 * README.md ("What we built with V3X", "For players"), docs/install.md ("Which plugins are on"),
 * docs/plugins.md and docs/sessions/2026-11-13-evening.md. Update them here when those change.
 */

export const REPO_URL = "https://github.com/kleeedolinux/v3x";
export const RELEASES_URL = "https://github.com/kleeedolinux/v3x/releases";
export const GAME_VERSION = "1.15.1";

/** README.md: "All numbers were measured on the author's PC (16 GB RAM, integrated GPU, Victoria 3 1.15.1)." */
export const MACHINE = { ram: "16 GB", gpu: "integrated", game: "1.15.1" };

export const measured = {
  meshLoad: { before: 37.5, after: 6.7, unit: "s", types: 2118, plugin: "lazy_mesh", shipped: "off" as const },
  debugSave: { before: 156, after: 26, unit: "MB", plugin: "enhance", shipped: "off" as const },
  fileLookups: { misses: "3.44 M", cost: "103.8 s", predicted: "94.4 %", mismatches: 0, mods: 23 },
  memReturned: { mib: "1,548 MiB", runs: 16, session: "1 h 44 m" },
};

type L = { en: string; pt: string };
export interface ShippedPlugin { id: string; on: boolean; what: L }

/** docs/install.md "Which plugins are on" + one-line plain descriptions from docs/plugins.md. */
export const shipped: ShippedPlugin[] = [
  { id: "mod_menu", on: true, what: {
    en: "The <b>V3X</b> button on the main menu, with a checkbox for each part.",
    pt: "O botão <b>V3X</b> no menu principal, com uma caixa de seleção para cada parte." } },
  { id: "keep_history", on: true, what: {
    en: "Keeps the GDP, population and other graphs of a country you stop playing; AI countries are recorded as often as yours (every 28 days).",
    pt: "Mantém os gráficos de PIB, população e outros de um país que você deixa de jogar; países da IA são registrados com a mesma frequência que o seu (a cada 28 dias)." } },
  { id: "mem_tuner", on: true, what: {
    en: "Tunes the game's memory use, gives memory back after loads, keeps a 128 MB emergency reserve and explains an out-of-memory error in the log.",
    pt: "Ajusta o uso de memória do jogo, devolve memória depois dos carregamentos, guarda uma reserva de emergência de 128 MB e explica no log um erro de falta de memória." } },
  { id: "frame_pacer", on: true, what: {
    en: "The frame cap sleeps instead of running a core at 100 %; lower frame rate when paused, idle, in the background or minimised.",
    pt: "O limite de quadros dorme em vez de manter um núcleo a 100 %; menos quadros quando pausado, parado, em segundo plano ou minimizado." } },
  { id: "cpu_sched", on: true, what: {
    en: "On CPUs with performance and efficiency cores, keeps the game's main thread on the performance cores. Does nothing on other CPUs.",
    pt: "Em processadores com núcleos de desempenho e de eficiência, mantém a thread principal do jogo nos núcleos de desempenho. Não faz nada nos outros." } },
  { id: "crash_logger", on: true, what: {
    en: "Writes <code>crash.log</code> when something goes wrong: the file to send with a bug report.",
    pt: "Escreve o <code>crash.log</code> quando algo dá errado: o arquivo para enviar junto com um relato de erro." } },
  { id: "ironman_console", on: true, what: {
    en: "Lets the console work in ironman (multiplayer stays blocked). Turn it off if achievements matter to you.",
    pt: "Libera o console no modo ironman (o multijogador continua bloqueado). Desligue se as conquistas importam para você." } },
  { id: "gfx_driver", on: true, what: {
    en: "Asks the graphics driver not to add its own threading. The effect on frame rate is not measured yet.",
    pt: "Pede ao driver de vídeo que não crie threads próprias. O efeito nos quadros por segundo ainda não foi medido." } },
  { id: "log_filter", on: true, what: {
    en: "Drops repeated, noisy lines from the engine's logs.",
    pt: "Remove linhas repetidas e ruidosas dos logs do motor." } },
  { id: "fast_load", on: true, what: {
    en: "Measures loading and tells the other parts when a load starts and ends.",
    pt: "Mede o carregamento e avisa as outras partes quando um carregamento começa e termina." } },
  { id: "content_packs", on: true, what: {
    en: "Lets files in <code>binaries\\v3x\\content_packs\\</code> replace game files without a mod. Needed by the V3X button.",
    pt: "Permite que arquivos em <code>binaries\\v3x\\content_packs\\</code> substituam arquivos do jogo sem um mod. Necessário para o botão V3X." } },
  { id: "lazy_mesh", on: false, what: {
    en: "Loads the 3D models your game uses at start and the rest on first use: 37.5 s down to 6.7 s on the author's PC.",
    pt: "Carrega no início os modelos 3D que o seu jogo usa e o resto no primeiro uso: de 37,5 s para 6,7 s no PC do autor." } },
  { id: "loc_fallback", on: false, what: {
    en: "Fills text that a mod lacks in your language with the mod's English text.",
    pt: "Preenche com o texto em inglês do mod as linhas que faltam no seu idioma." } },
  { id: "enhance", on: false, what: {
    en: "In debug mode, saves in the normal compressed format: 156 MB down to 26 MB per autosave.",
    pt: "No modo debug, salva no formato comprimido normal: de 156 MB para 26 MB por autosave." } },
  { id: "console_plus", on: false, what: {
    en: "Tab completion in the console: country tags, states, laws.",
    pt: "Completar com Tab no console: tags de países, estados, leis." } },
  { id: "asset_stream", on: false, what: {
    en: "Forces the game's own texture streaming on (every quality preset turns it off).",
    pt: "Força o streaming de texturas do próprio jogo (todas as predefinições de qualidade o desligam)." } },
  { id: "sim_opt", on: false, what: {
    en: "Simulation diagnostics. Its optional throttle changes the simulation: back up saves first.",
    pt: "Diagnóstico da simulação. A opção de throttle altera a simulação: faça backup dos saves antes." } },
];
