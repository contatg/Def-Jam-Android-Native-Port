# Android v8 — FPS no launcher e D-pad agrupado

Continuação do v7 que o usuário testou em 22/10/2026. O log enviado registra
1 CPU guest, limitador original e ativação posterior do limite de 30 FPS.
O usuário aprovou o resultado geral e pediu duas mudanças de interface.

## Mudanças

- O launcher inicial tem um slider de limite de FPS de 30 a 165, em passos de 1 FPS. A seleção fica salva e é aplicada na próxima abertura do jogo. Uma preferência antiga 0 (original) migra para 60; qualquer seleção válida de 30 a 165 é preservada. O medidor do slider mostra FPS, sem porcentagem.
- Os controles de FPS foram retirados do menu durante o jogo. START, CONTROLES e MENU continuam disponíveis; MENU abre F1 e CONTROLES abre a edição sem trocar de Activity.
- O código nativo agora aceita o intervalo inteiro 30..165, incluindo 31, 45, 60, 90, 120, 144 e 165. A opção é um teto de apresentações; não muda o timestep do jogo nem desbloqueia o timer interno de 60 FPS. Selecionar 165 não garante atingir 165. Um teto baixo ainda pode afetar o ritmo, como documentado no v7.
- O D-pad é um grupo único no editor. Tocar em qualquer seta ou no centro seleciona o conjunto. Arrastar move as quatro; +/− redimensionam o conjunto mantendo as distâncias; Mostrar/ocultar aplica às quatro. A lista Escolher apresenta apenas um item D-PAD.
- Durante a partida, cada direção mantém sua ação e as diagonais enviam as duas direções simultaneamente. O centro permanece neutro. O contorno de seleção e a limitação às bordas abrangem o conjunto inteiro, sem separar as setas.
- A disposição antiga das quatro setas é migrada para um conjunto centrado na média das posições salvas. Os demais botões, tamanhos, visibilidades e mapeamentos são preservados. O grupo é salvo no mesmo controls-v7.json com novos campos dpad_v8/dpad_x/dpad_y/dpad_scale/dpad_visible e escrita AtomicFile.
- As opções de CPU, a arte dos botões de luta e as mudanças de superfície, Turnip e fullscreen continuam como no v7.

## Validação executada

- 3 testes passaram. A geometria Java de produção foi compilada com javac 17 e executada sem Android: movimento conjunto, escala, cantos da tela, visibilidade, quatro direções, diagonais, centro neutro e valores não finitos.
- Os testes C de produção passaram com GCC C11, -Wall -Wextra -Werror: afinidade da versão anterior e temporizador generalizado (30, 31, 45, 60, 90, 120, 144, 165), limites/valores inválidos, retomada e espera interrompida. A bridge JNI usa tipos mínimos e relógio controlado no teste, não o Android real.
- Os 6 arquivos Java do app passaram pelo parser oficial javac 17. Isso verifica sintaxe, não resolução de tipos Android nem construção do APK.
- O pacote e todos os hashes de payload foram conferidos contra a base v7 e o resultado v8.
- Não houve build Android/NDK/Gradle, teste visual por toque ou teste em celular neste ambiente. O bom resultado do v7 é relato do usuário; o v8 ainda precisa ser instalado e validado.
- Não foi feita investigação de desempenho ou correção dos avisos antigos de arquivos/ICALL no log: este patch atende somente às duas alterações solicitadas.

## Aplicar

Aplicar-v8.ps1 requer o v7 aplicado. Ele confere todos os arquivos antes de escrever,
faz backup, verifica as cópias e tenta restaurar se alguma cópia falhar.
Use -Build -ProjectRoot <projeto> -DataDir "C:\Games\DefJamFFNY-data" -Variant Release
para chamar a compilação incremental já existente. Não aplique os instaladores v6/v7 por cima do v8.

## Conferir no aparelho

1. No launcher, mover o slider para 30, 60, 90, 120 e 165. Conferir o texto e reabrir o app para verificar a preferência salva. O valor escolhido só passa a valer ao iniciar novamente o jogo.
2. No editor, selecionar D-PAD e arrastar qualquer seta. Conferir movimento conjunto, +/−, bordas, ocultação do grupo e Salvar/Cancelar. Reiniciar e conferir persistência.
3. Testar direções, diagonais e centro neutro no jogo/menu. Confirmar que os botões de luta não mudaram.
4. Conferir que o menu durante a partida já não tem seletor de FPS. START e a edição no jogo continuam acessíveis.
5. Manter 1 núcleo para comparar os limites. Conferir [ANDROID] present limit e frame pacing no defjam-native.log e o ritmo/áudio da luta. FPS escolhido é um teto, não aceleração automática.
