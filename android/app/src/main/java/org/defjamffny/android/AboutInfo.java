package org.defjamffny.android;

/** App-bundled attribution. Never loaded from preferences or shared storage. */
final class AboutInfo {
    private AboutInfo() {}

    static final String TITLE = "Def Jam Android Native Port";
    static final String MAINTAINER = "contatg";
    static final String GITHUB = "https://github.com/contatg/Def-Jam-Android-Native-Port";

    static final String ONLINE_UPSTREAM = "https://github.com/sweeax-exe/ffny-online";

    static String credits(String version) {
        if (UiText.isSpanish()) return TITLE + "\nVersión: " + version
                + "\n\nPort de Android mantenido por " + MAINTAINER + "\n\nGitHub:\n" + GITHUB
                + "\n\nMultijugador en línea de FFNY Online, creado por sweeax-exe y sus colaboradores.\n" + ONLINE_UPSTREAM
                + "\n\nProyecto de recompilación original: Vlad Yanhursky y colaboradores."
                + "\ngithub.com/vyanhursky/DefJamFFNY-recomp"
                + "\n\nRuntime: xboxrecomp y colaboradores; componentes derivados de xemu."
                + "\nConsulta las licencias de terceros para los avisos originales."
                + "\n\nRequiere todos los datos extraídos de tu propio disco/ISO original de Xbox."
                + " Este proyecto no incluye la imagen del disco ni los datos originales del juego."
                + "\n\nProyecto comunitario no oficial. El juego, los personajes, la música, los logotipos"
                + " y las ilustraciones pertenecen a sus respectivos propietarios.\n";
        if (UiText.isPortuguese()) return TITLE + "\nVersão: " + version
                + "\n\nPort Android mantido por " + MAINTAINER + "\n\nGitHub:\n" + GITHUB
                + "\n\nMultijogador online do FFNY Online, criado por sweeax-exe e seus colaboradores.\n" + ONLINE_UPSTREAM
                + "\n\nRecompilação original: Vlad Yanhursky e colaboradores."
                + "\nRuntime: xboxrecomp e colaboradores; componentes derivados do xemu."
                + "\nConsulte as licenças de terceiros."
                + "\n\nExige os arquivos completos extraídos do seu disco/ISO original de Xbox."
                + " O jogo original e seus arquivos de dados não são fornecidos."
                + "\n\nProjeto não oficial. Jogo, personagens, música, marcas e arte pertencem aos respectivos donos.\n";
        return TITLE + "\nVersion: " + version
                + "\n\nAndroid port maintained by " + MAINTAINER
                + "\n\nGitHub:\n" + GITHUB
                + "\n\nOnline multiplayer from FFNY Online, created by sweeax-exe and its contributors.\n" + ONLINE_UPSTREAM
                + "\n\nOriginal recompilation project: Vlad Yanhursky and contributors"
                + "\ngithub.com/vyanhursky/DefJamFFNY-recomp"
                + "\n\nRuntime: xboxrecomp and contributors; xemu-derived components."
                + "\nSee Third-party licenses for preserved upstream notices."
                + "\n\nRequires the complete data extracted from your own original Xbox disc/ISO."
                + " The disc image and original game data are not supplied by this project."
                + "\n\nUnofficial community project. The game, characters, music, logos and artwork"
                + " belong to their respective owners.\n";
    }

    static String setup() {
        if (UiText.isSpanish()) return
                "1. Usa Extraer ISO de Xbox en el launcher para extraer tu propia ISO/XISO en el teléfono,"
                + " o extrae el disco con una herramienta compatible con Xbox."
                + " Se admite la versión original de Xbox de EE. UU. (NTSC-U). No basta con renombrar la ISO."
                + "\n\n2. En la raíz del ALMACENAMIENTO INTERNO COMPARTIDO, crea DefJamFFNY/game."
                + " No es la raíz del sistema Android ni Android/data. No requiere acceso root."
                + "\n\n3. Copia TODOS los archivos y carpetas extraídos a game, conservando los nombres y la estructura."
                + " El ejecutable debe estar en:\n/storage/emulated/0/DefJamFFNY/game/default.xbe"
                + "\n\nEl nombre es default.xbe, no defalt.xbe. No añadas otra carpeta entre game y default.xbe."
                + " El ejecutable por sí solo no es suficiente."
                + "\n\nConserva fighter.xml, gamedata.xml, gconfig.xml, venue.xml y las carpetas assets/"
                + " (recursos), audio/ (audio), fonts/ (fuentes), motion/ (movimientos), movies/"
                + " (vídeos), screens/ (pantallas), junto con TODOS los demás archivos del disco."
                + " Conserva todas sus subcarpetas."
                + "\n\n4. Permite el acceso a archivos. El launcher debe indicar Juego encontrado."
                + " Si no lo encuentra, revisa la ruta, los nombres, las carpetas adicionales y el permiso."
                + " Detectar default.xbe no verifica el resto de la extracción."
                + "\n\nEl port crea settings.ini y defjam-native.log en DefJamFFNY."
                + " El registro se reemplaza al iniciar el juego. Conserva save/ y haz una copia de seguridad"
                + " antes de actualizar o cambiar de dispositivo. No borres los datos ni desinstales para actualizar."
                + "\n\nMás información:\n" + GITHUB + "/blob/main/SETUP.md";
        if (!UiText.isPortuguese()) return SETUP;
        return "1. No launcher, use Importar ISO de Xbox para extrair sua ISO/XISO diretamente no celular, ou extraia seu disco com uma ferramenta compatível."
                + " A versão suportada é USA (NTSC-U). Não basta renomear a ISO."
                + "\n\n2. Na raiz do ARMAZENAMENTO INTERNO COMPARTILHADO, crie DefJamFFNY/game."
                + " Não é a raiz do sistema Android nem a pasta Android/data. Não exige root."
                + "\n\n3. Copie TODOS os arquivos e pastas extraídos para game, mantendo nomes e estrutura."
                + " O executável deve estar em:\n/storage/emulated/0/DefJamFFNY/game/default.xbe"
                + "\n\nNão crie outra pasta entre game e default.xbe. O executável sozinho não basta."
                + "\n\nMantenha fighter.xml, gamedata.xml, gconfig.xml, venue.xml e as pastas assets/"
                + " (recursos), audio/ (áudio), fonts/ (fontes), motion/ (movimentos), movies/"
                + " (vídeos), screens/ (telas), além de TODOS os demais arquivos do disco."
                + "\n\n4. Permita acesso aos arquivos no app. O launcher deve indicar Jogo encontrado."
                + " Verifique caminho, nomes, pastas extras e permissão se não encontrar o jogo."
                + "\n\nsettings.ini e defjam-native.log são criados pelo port em DefJamFFNY."
                + " O log é substituído a cada nova abertura. Preserve a pasta save/ e faça backup"
                + " antes de atualizar ou trocar de aparelho. Não desinstale nem apague os dados"
                + " do app apenas para atualizar.\n\nMais informações:\n" + GITHUB + "/blob/main/SETUP.md";
    }

    static final String SETUP =
            "1. Use Import Xbox ISO in the launcher to extract your own ISO/XISO directly on the phone, or extract your disc with an Xbox-compatible tool."
            + " The supported dump is the USA (NTSC-U) original Xbox version."
            + " Do not rename the ISO or copy it directly into the app folder."
            + "\n\n2. In the top level of INTERNAL SHARED STORAGE, create DefJamFFNY/game."
            + " This is not the Android system root or Android/data. Root access is not required."
            + "\n\n3. Copy ALL extracted disc contents into game, keeping every name and subfolder."
            + " The required executable is:\n/storage/emulated/0/DefJamFFNY/game/default.xbe"
            + "\n\nSpell it default.xbe, not defalt.xbe. Do not add another game-title directory"
            + " between game and default.xbe. The executable alone is not enough."
            + "\n\nExpected disc files include default.xbe, fighter.xml, gamedata.xml, gconfig.xml,"
            + " venue.xml, plus assets/, audio/, fonts/, motion/, movies/ and screens/."
            + " Keep all other extracted files too."
            + "\n\nassets/: game assets. audio/: audio data. fonts/: font resources."
            + " motion/: motion data. movies/: video resources. screens/: screen resources."
            + " Preserve their full nested directory structure."
            + "\n\n4. Allow the app's requested file access. The launcher should show Game found."
            + " If it does not, check the path, spelling, nesting and permission."
            + " Finding default.xbe does not verify the rest of the extraction."
            + "\n\nThe port creates settings.ini and defjam-native.log in DefJamFFNY."
            + " The log is replaced on a new game launch. Keep your existing save/ folder;"
            + " back up the data root before updating or moving devices."
            + " Never clear app data or uninstall just to update."
            + "\n\nFull instructions and additional information:\n" + GITHUB + "/blob/main/SETUP.md";
}
