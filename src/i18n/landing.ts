/** Text of the landing page in both languages. HTML is allowed (trusted, written here). */
import { RELEASES_URL } from "../data/facts";

export interface LandingText {
  title: string; description: string;
  kicker: string; h1: string; lede: string;
  download: string; downloadSub: string; guide: string; promise: string;
  ledgerCaption: string; ledgerHead: [string, string, string];
  ledgerRows: { what: string; tag: string; before: string; after: string }[];
  ledgerNote: string;
  s1: string; register: { head: string; on: boolean; body: string }[]; stampOn: string; stampOpt: string;
  s2: string; s2intro: string;
  fig1: string; fig2: string; figBefore: string; figAfter: string; figNote: string; decimal: "," | ".";
  method: string;
  s3: string; s3intro: string; cmpHead: [string, string, string]; cmp: [string, string, string][]; plate1: string;
  s4: string; steps: { b: string; t: string }[]; preNote: string;
  s5: string; offHead: [string, string]; off: [string, string][];
  s6: string; qa: { q: string; a: string }[];
  s7: string; modders: string; modLink: string; plate2: string;
  labels: [string, string, string, string, string, string, string];
}

export const en: LandingText = {
  title: "V3X: Victoria 3 script extender with engine fixes and faster loading",
  description: "V3X is a script extender for Victoria 3: engine bug fixes, faster 3D-model loading, fewer out-of-memory crashes, kept GDP graphs. Works with your mods. Single player, Windows.",
  kicker: "A script extender for Victoria 3 · it works under the game, not in it",
  h1: "Victoria 3, with its engine <em>looked after</em>.",
  lede: "V3X is an add-on that runs underneath the game. It fixes bugs in the engine, takes work out of loading, keeps your graphs, and writes a clear report when something breaks. Your mods and your playset stay exactly as they are. One installer puts it in; one click takes it out.",
  download: "Download V3X-Setup.exe", downloadSub: "Windows x64 · for Victoria 3 1.15.1",
  guide: "Read the players' guide",
  promise: "No admin rights. No game file is changed. Single player only.",
  ledgerCaption: "The ledger: measured on the author's PC",
  ledgerHead: ["", "before", "with V3X"],
  ledgerRows: [
    { what: "Loading pass over 2,118 kinds of 3D model", tag: "lazy_mesh · optional", before: "37.5 s", after: "6.7 s" },
    { what: "One autosave in debug mode", tag: "enhance · optional", before: "156 MB", after: "26 MB" },
    { what: "History graphs after you switch country", tag: "keep_history · on", before: "wiped", after: "kept" },
    { what: "New Game on a PC short of commit memory", tag: "mem_tuner · on", before: "crashed", after: "loads¹" },
  ],
  ledgerNote: "16 GB RAM, integrated GPU, Victoria 3 1.15.1. Yours will differ. ¹ With a bigger Windows pagefile; the author then played a 4-hour session.",
  s1: "What it does for you",
  stampOn: "on", stampOpt: "optional",
  register: [
    { head: "Graphs that remember", on: true, body: "When you stop playing a country, the game wipes its GDP, population and other history graphs. V3X keeps them, and records AI countries as often as yours: every 28 days instead of every 120." },
    { head: "Fewer out-of-memory crashes", on: true, body: "The New Game crash came from Windows running out of commit memory while RAM was still free. V3X makes the game reserve less, returns memory after loads and keeps a 128 MB emergency reserve. If memory still runs out, the log says to enlarge the pagefile." },
    { head: "A processor that can rest", on: true, body: "The frame cap used to keep one core busy at 100 % just waiting. V3X sleeps instead, and lowers the frame rate while you are paused, idle or in another window." },
    { head: "The right cores on new Intel CPUs", on: true, body: "On processors with performance and efficiency cores, the game's main thread is kept on the performance cores. On other processors nothing changes." },
    { head: "A V3X button on the main menu", on: true, body: "Each part of V3X has a checkbox. Untick what you do not want; it applies the next time you start the game." },
    { head: "A crash report you can send", on: true, body: "If the game crashes, <code>crash.log</code> records where and why, in a form a developer can act on." },
    { head: "Console in ironman", on: true, body: "The developer console works in ironman. Multiplayer stays blocked. If achievements matter to you, turn this one off." },
    { head: "3D models loaded when needed", on: false, body: "Instead of loading every kind of 3D model up front, V3X loads the ones your game uses and the rest when first seen. 37.5 s became 6.7 s on the author's PC." },
    { head: "English text where a mod has none in your language", on: false, body: "Mods without a translation show raw text keys. V3X can fill those lines with the mod's English text." },
    { head: "Smaller saves in debug mode", on: false, body: "Debug mode writes saves as plain text. V3X keeps the normal compressed format: 156 MB became 26 MB per autosave." },
  ],
  s2: "Measured, not promised",
  s2intro: "Every fix went the same way: measure with V3X's own tools, find the cause in the engine, fix it in a plugin, check it against the real game files, then measure again in the game.",
  fig1: "Fig. 1. The loading pass over all 3D models, in seconds", fig2: "Fig. 2. One autosave with debug mode on, in megabytes",
  figBefore: "Before", figAfter: "With V3X",
  figNote: "Measured on the author's PC (16 GB RAM, integrated GPU, Victoria 3 1.15.1). Fig. 1 with <code>lazy_mesh</code> in <code>lazy</code> mode during a 4-hour session, Fig. 2 with <code>enhance</code>. Both parts are optional and off after install.",
  decimal: ".",
  method: "In one session with 23 mods, the engine made 3.44 million failed file lookups, costing 103.8 s of thread time. A V3X index predicted 94.4 % of them with 0 mismatches. Using it to skip those lookups is the next step, not yet a result.",
  s3: "Is it a mod? Does it replace mine?",
  s3intro: "No. Mods change <em>what is in the game</em>: events, buildings, laws, the interface. V3X changes <em>how the engine runs</em>: loading, memory, saves, crashes. Both work at the same time.",
  cmpHead: ["", "A Workshop mod", "V3X"],
  cmp: [
    ["Made of", "text and data files", "a program that loads with the game"],
    ["Changes", "content: what is in the game", "the engine: loading, memory, saves, crashes"],
    ["Your mod list", "is the mod list", "untouched; V3X is not in it"],
    ["Multiplayer", "yes", "no, single player only"],
  ],
  plate1: "You start Victoria 3; V3X loads with it as one extra file (version.dll); its plugins fix the engine; you play. Delete that file and the game is vanilla again. Diagram from the V3X repository.",
  s4: "Install in four steps",
  steps: [
    { b: "Download", t: `Get <code>V3X-Setup.exe</code> from the <a href="${RELEASES_URL}">Releases page</a>. Windows may warn because the file is new and unsigned: choose <em>More info › Run anyway</em> if you trust the source.` },
    { b: "Close the game, run setup", t: "Setup finds Victoria 3 through Steam. If it cannot, pick the <code>Victoria 3</code> folder (Steam: right-click the game › Manage › Browse local files)." },
    { b: "Check, then install", t: "The window should say <code>Game version: release/1.15.1 (supported)</code>. Click <b>Install V3X</b>. No admin rights needed." },
    { b: "Play", t: "Start the game as usual. A small <em>V3X console</em> window opens beside it and lists what loaded. You can hide it later." },
  ],
  preNote: "V3X is pre-release. If the Releases page does not list <code>V3X-Setup.exe</code> yet, the first public build is not out; the <a href=\"/guide/#without-setup\">players' guide</a> explains the other way in.",
  s5: "Turning it off",
  offHead: ["You want to", "Do this"],
  off: [
    ["Turn one part off", "Main menu › <b>V3X</b>, untick it. Or write <code>name=0</code> under <code>[plugins]</code> in <code>binaries\\v3x\\v3x.ini</code>. It applies at the next start."],
    ["Play once without V3X (for example online)", "Rename <code>binaries\\version.dll</code> to <code>version.dll.off</code>. Rename it back afterwards."],
    ["Remove V3X", "Run <code>V3X-Setup.exe</code> again and choose <b>Uninstall V3X</b>. It removes exactly the files it installed and puts back anything it replaced."],
    ["Remove it without the setup file", "Delete <code>binaries\\version.dll</code> and the folder <code>binaries\\v3x</code>. That is the whole uninstall."],
  ],
  s6: "Questions players ask",
  qa: [
    { q: "Is it safe?", a: "V3X changes no game file. It is one extra file next to the game, so Steam's <em>Verify integrity of game files</em> finds nothing changed and leaves V3X in place. Every part can be turned off on its own. After a game update, a part whose engine code changed switches itself off and writes why in <code>binaries\\v3x\\v3x.log</code>, instead of guessing." },
    { q: "Why does my antivirus or Windows warn me?", a: "V3X loads through a file called <code>version.dll</code> next to the game, a common technique that some antivirus programs flag on principle. The setup file is new and not signed, so SmartScreen may warn too. All the source code is public, and you can build it yourself." },
    { q: "Does it break achievements or ironman?", a: "V3X does not touch achievement code or DLC ownership, and unlocks nothing. But one part that is on by default, <code>ironman_console</code>, lets the console work in ironman. If achievements matter to you, turn it off and leave the optional simulation features off." },
    { q: "Can I play multiplayer with it?", a: "No. V3X is for single player: other players would be running different code. Before you play online, uninstall it or rename <code>binaries\\version.dll</code>." },
    { q: "Will my Workshop mods still work?", a: "Yes. V3X does not touch your mod list. A mod that replaces the main menu hides the V3X button, but <code>v3x.ini</code> still works." },
    { q: "Can it break my saves?", a: "Parts that change the simulation are off by default. Back up your saves before you try anything new." },
    { q: "What happens when Victoria 3 updates?", a: "Parts whose engine code changed switch themselves off until V3X is updated. V3X targets 1.15.1; setup tells you if your game version is different and installs only if you insist." },
    { q: "How do I uninstall it?", a: "Run <code>V3X-Setup.exe</code> and choose <b>Uninstall V3X</b>, or delete <code>binaries\\version.dll</code> and <code>binaries\\v3x</code>. See <a href=\"#off\">Turning it off</a>." },
    { q: "Linux, Proton, Steam Deck?", a: "Untested. It may need the launch option <code>WINEDLLOVERRIDES=\"version=n,b\"</code>." },
    { q: "Who makes it?", a: "An unofficial fan project, not affiliated with or endorsed by Paradox Interactive. The code is on <a href=\"https://github.com/kleeedolinux/v3x\">GitHub</a>." },
  ],
  s7: "For modders: a platform underneath",
  modders: "Every part above is a plugin built on the V3X API: a plain-C interface to find engine code that survives patches, hook it safely, read live data, add console commands and react to loads and saves. The SDK tests your plugin against the real game files before you start the game.",
  modLink: "Open the modders' documentation",
  plate2: "The V3X platform: plugins use the SDK and the V3X API; the loader, version.dll, loads them into the engine, which stays unchanged on disk. Diagram from the V3X repository.",
  labels: ["What it does", "Measured", "Mods", "Install", "Off switch", "Questions", "Modders"],
};

export const pt: LandingText = {
  title: "V3X: script extender para Victoria 3, com correções no motor e carregamento mais rápido",
  description: "V3X é um script extender para Victoria 3: corrige bugs do motor, carrega modelos 3D mais rápido, evita travamentos por falta de memória. Funciona com seus mods. Um jogador, Windows.",
  kicker: "Um script extender para Victoria 3 · funciona por baixo do jogo, não dentro dele",
  h1: "Victoria 3, com o motor <em>bem cuidado</em>.",
  lede: "V3X é um complemento que roda por baixo do jogo. Corrige bugs do motor, tira trabalho do carregamento, preserva os seus gráficos e escreve um relatório claro quando algo quebra. Os seus mods e a sua playset continuam exatamente como estão. Um instalador coloca; um clique tira.",
  download: "Baixar V3X-Setup.exe", downloadSub: "Windows x64 · para Victoria 3 1.15.1",
  guide: "Ler o guia do jogador",
  promise: "Não precisa de administrador. Nenhum arquivo do jogo é alterado. Só para um jogador.",
  ledgerCaption: "O livro-caixa: medido no PC do autor",
  ledgerHead: ["", "antes", "com V3X"],
  ledgerRows: [
    { what: "Carregamento dos 2.118 tipos de modelo 3D", tag: "lazy_mesh · opcional", before: "37,5 s", after: "6,7 s" },
    { what: "Um autosave no modo debug", tag: "enhance · opcional", before: "156 MB", after: "26 MB" },
    { what: "Gráficos de histórico depois de trocar de país", tag: "keep_history · ligado", before: "apagados", after: "mantidos" },
    { what: "Novo Jogo num PC sem memória de commit", tag: "mem_tuner · ligado", before: "travava", after: "carrega¹" },
  ],
  ledgerNote: "16 GB de RAM, GPU integrada, Victoria 3 1.15.1. No seu PC será diferente. ¹ Com um arquivo de paginação maior no Windows; o autor jogou depois uma sessão de 4 horas.",
  s1: "O que ele faz por você",
  stampOn: "ligado", stampOpt: "opcional",
  register: [
    { head: "Gráficos que não se perdem", on: true, body: "Quando você deixa de jogar com um país, o jogo apaga os gráficos de PIB, população e outros. O V3X os mantém e registra os países da IA com a mesma frequência que o seu: a cada 28 dias em vez de 120." },
    { head: "Menos travamentos por falta de memória", on: true, body: "O travamento no Novo Jogo vinha do Windows ficando sem memória de commit enquanto ainda havia RAM livre. O V3X faz o jogo reservar menos, devolve memória depois dos carregamentos e guarda 128 MB de reserva de emergência. Se ainda faltar memória, o log diz para aumentar o arquivo de paginação." },
    { head: "Um processador que descansa", on: true, body: "O limite de quadros mantinha um núcleo a 100 % só esperando. O V3X dorme no lugar disso e reduz os quadros quando você pausa, fica parado ou troca de janela." },
    { head: "Os núcleos certos nos Intel novos", on: true, body: "Em processadores com núcleos de desempenho e de eficiência, a thread principal do jogo fica nos núcleos de desempenho. Nos outros processadores nada muda." },
    { head: "Um botão V3X no menu principal", on: true, body: "Cada parte do V3X tem uma caixa de seleção. Desmarque o que não quiser; vale na próxima vez que abrir o jogo." },
    { head: "Um relatório de erro para enviar", on: true, body: "Se o jogo fechar com erro, o <code>crash.log</code> registra onde e por quê, de um jeito que um desenvolvedor consegue usar." },
    { head: "Console no ironman", on: true, body: "O console de desenvolvedor funciona no ironman. O multijogador continua bloqueado. Se as conquistas importam para você, desligue esta parte." },
    { head: "Modelos 3D carregados quando precisa", on: false, body: "Em vez de carregar todos os tipos de modelo 3D logo no início, o V3X carrega os que o seu jogo usa e o resto quando aparecem pela primeira vez. 37,5 s viraram 6,7 s no PC do autor." },
    { head: "Texto em inglês onde o mod não tem o seu idioma", on: false, body: "Mods sem tradução mostram as chaves de texto cruas. O V3X pode preencher essas linhas com o texto em inglês do próprio mod." },
    { head: "Saves menores no modo debug", on: false, body: "O modo debug grava os saves como texto puro. O V3X mantém o formato comprimido normal: 156 MB viraram 26 MB por autosave." },
  ],
  s2: "Medido, não prometido",
  s2intro: "Toda correção seguiu o mesmo caminho: medir com as ferramentas do próprio V3X, achar a causa no motor, corrigir num plugin, conferir contra os arquivos reais do jogo e medir de novo dentro do jogo.",
  fig1: "Fig. 1. Carregamento de todos os modelos 3D, em segundos", fig2: "Fig. 2. Um autosave com o modo debug ligado, em megabytes",
  figBefore: "Antes", figAfter: "Com V3X",
  figNote: "Medido no PC do autor (16 GB de RAM, GPU integrada, Victoria 3 1.15.1). Fig. 1 com <code>lazy_mesh</code> no modo <code>lazy</code> numa sessão de 4 horas, Fig. 2 com <code>enhance</code>. As duas partes são opcionais e vêm desligadas.",
  decimal: ",",
  method: "Numa sessão com 23 mods, o motor fez 3,44 milhões de buscas de arquivo que falharam, custando 103,8 s de tempo de thread. Um índice do V3X previu 94,4 % delas com 0 erros. Usá-lo para pular essas buscas é o próximo passo, ainda não é um resultado.",
  s3: "É um mod? Ele substitui os meus?",
  s3intro: "Não. Mods mudam <em>o que existe no jogo</em>: eventos, construções, leis, a interface. O V3X muda <em>como o motor funciona</em>: carregamento, memória, saves, travamentos. Os dois funcionam ao mesmo tempo.",
  cmpHead: ["", "Um mod da Oficina", "V3X"],
  cmp: [
    ["Feito de", "arquivos de texto e dados", "um programa que carrega junto com o jogo"],
    ["Muda", "conteúdo: o que existe no jogo", "o motor: carregamento, memória, saves, travamentos"],
    ["Sua lista de mods", "é a lista de mods", "intacta; o V3X não está nela"],
    ["Multijogador", "sim", "não, só um jogador"],
  ],
  plate1: "Você abre Victoria 3; o V3X carrega junto como um arquivo a mais (version.dll); os plugins corrigem o motor; você joga. Apague esse arquivo e o jogo volta a ser o original. Diagrama do repositório do V3X (em inglês).",
  s4: "Instale em quatro passos",
  steps: [
    { b: "Baixe", t: `Pegue o <code>V3X-Setup.exe</code> na <a href="${RELEASES_URL}">página de Releases</a>. O Windows pode avisar porque o arquivo é novo e não assinado: escolha <em>Mais informações › Executar assim mesmo</em> se confia na origem.` },
    { b: "Feche o jogo, rode o instalador", t: "O instalador acha Victoria 3 pela Steam. Se não achar, escolha a pasta <code>Victoria 3</code> (na Steam: botão direito no jogo › Gerenciar › Ver arquivos locais)." },
    { b: "Confira e instale", t: "A janela deve mostrar <code>Game version: release/1.15.1 (supported)</code>. Clique em <b>Install V3X</b>. Não precisa de administrador." },
    { b: "Jogue", t: "Abra o jogo como sempre. Uma pequena janela <em>V3X console</em> aparece ao lado e mostra o que carregou. Dá para escondê-la depois." },
  ],
  preNote: "O V3X está em pré-lançamento. Se a página de Releases ainda não tiver o <code>V3X-Setup.exe</code>, a primeira versão pública ainda não saiu; o <a href=\"/pt-br/guia/#sem-instalador\">guia do jogador</a> explica o outro caminho.",
  s5: "Como desligar",
  offHead: ["Você quer", "Faça isto"],
  off: [
    ["Desligar uma parte", "Menu principal › <b>V3X</b>, desmarque. Ou escreva <code>nome=0</code> embaixo de <code>[plugins]</code> em <code>binaries\\v3x\\v3x.ini</code>. Vale na próxima abertura."],
    ["Jogar uma vez sem o V3X (por exemplo online)", "Renomeie <code>binaries\\version.dll</code> para <code>version.dll.off</code>. Depois renomeie de volta."],
    ["Remover o V3X", "Rode o <code>V3X-Setup.exe</code> de novo e escolha <b>Uninstall V3X</b>. Ele remove exatamente os arquivos que instalou e devolve o que tiver substituído."],
    ["Remover sem o instalador", "Apague <code>binaries\\version.dll</code> e a pasta <code>binaries\\v3x</code>. A desinstalação é só isso."],
  ],
  s6: "Perguntas de jogadores",
  qa: [
    { q: "É seguro?", a: "O V3X não altera nenhum arquivo do jogo. É um arquivo a mais ao lado do jogo, então a opção da Steam <em>Verificar integridade dos arquivos</em> não encontra nada alterado e deixa o V3X onde está. Cada parte pode ser desligada separadamente. Depois de uma atualização do jogo, a parte cujo código do motor mudou se desliga sozinha e escreve o motivo em <code>binaries\\v3x\\v3x.log</code>, em vez de arriscar." },
    { q: "Por que o antivírus ou o Windows reclamam?", a: "O V3X carrega por um arquivo chamado <code>version.dll</code> ao lado do jogo, uma técnica comum que alguns antivírus marcam por princípio. O instalador é novo e não assinado, então o SmartScreen também pode avisar. Todo o código-fonte é público e você pode compilá-lo você mesmo." },
    { q: "Estraga as conquistas ou o ironman?", a: "O V3X não mexe no código das conquistas nem na posse de DLCs, e não desbloqueia nada. Mas uma parte ligada por padrão, <code>ironman_console</code>, libera o console no ironman. Se as conquistas importam para você, desligue-a e deixe desligadas as opções que mudam a simulação." },
    { q: "Dá para jogar multijogador com ele?", a: "Não. O V3X é para um jogador: os outros jogadores estariam rodando um código diferente. Antes de jogar online, desinstale ou renomeie <code>binaries\\version.dll</code>." },
    { q: "Meus mods da Oficina continuam funcionando?", a: "Sim. O V3X não mexe na sua lista de mods. Um mod que substitui o menu principal esconde o botão V3X, mas o <code>v3x.ini</code> continua funcionando." },
    { q: "Pode estragar meus saves?", a: "As partes que mudam a simulação vêm desligadas. Faça backup dos saves antes de experimentar algo novo." },
    { q: "E quando Victoria 3 atualizar?", a: "As partes cujo código do motor mudou se desligam sozinhas até o V3X ser atualizado. O V3X é feito para a 1.15.1; o instalador avisa se a sua versão é outra e só instala se você insistir." },
    { q: "Como desinstalo?", a: "Rode o <code>V3X-Setup.exe</code> e escolha <b>Uninstall V3X</b>, ou apague <code>binaries\\version.dll</code> e <code>binaries\\v3x</code>. Veja <a href=\"#off\">Como desligar</a>." },
    { q: "Linux, Proton, Steam Deck?", a: "Não testado. Pode precisar da opção de inicialização <code>WINEDLLOVERRIDES=\"version=n,b\"</code>." },
    { q: "Quem faz?", a: "Um projeto de fãs, não oficial, sem vínculo nem endosso da Paradox Interactive. O código está no <a href=\"https://github.com/kleeedolinux/v3x\">GitHub</a>." },
  ],
  s7: "Para modders: uma plataforma por baixo",
  modders: "Cada parte acima é um plugin feito com a API do V3X: uma interface em C puro para achar código do motor que sobrevive a patches, interceptá-lo com segurança, ler dados ao vivo, criar comandos de console e reagir a carregamentos e saves. O SDK testa o seu plugin contra os arquivos reais do jogo antes mesmo de abrir o jogo.",
  modLink: "Abrir a documentação para modders (em inglês)",
  plate2: "A plataforma V3X: os plugins usam o SDK e a API do V3X; o carregador, version.dll, os coloca dentro do motor, que continua intacto no disco. Diagrama do repositório do V3X (em inglês).",
  labels: ["O que faz", "Medido", "Mods", "Instalar", "Desligar", "Perguntas", "Modders"],
};
