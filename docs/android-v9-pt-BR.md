# Android v9 — toque no menu do port e controles por contexto

## Pedido e evidência

O usuário pediu toque direto no menu do port, controles Xbox nos menus do jogo e a arte personalizada somente na luta. Também relatou que abrir com 5 núcleos não trouxe ganho.

O defjam-native.log enviado nesta rodada registra request=6, máscara permitida de 5 CPUs e limitador ativo de 30 FPS. Após carregar, muitos frames ficam em aproximadamente 33,4 ms; a espera de fence fica perto de 1–3 ms. O trecho vai até intMain/preStart.swf e não demonstra uma comparação controlada de luta em 1 vs 5 CPUs. Não foi demonstrado que os núcleos extras diminuem o custo da simulação.

## Mudanças

- Menu do port (F1/MENU): recebe toque direto. Uma View transparente envia o primeiro dedo pela entrada SDL existente, em coordenadas normalizadas. O adaptador SDL entrega down/move/up/cancel ao Dear ImGui; a conversão para pixels usa o framebuffer do overlay. O Android não gera um segundo clique de mouse para o mesmo dedo. Eventos consumidos não seguem para o controle do jogo. Ao fechar o menu, perder foco ou cancelar o toque, a pressão é liberada.
- Toque ajusta botões, abas, campos, sliders e barras de rolagem do port. Arraste a barra lateral para rolar; este patch não implementa gesto de rolagem inercial sobre qualquer área vazia.
- Menus do jogo: novo pad independente com A/B/X/Y, LT/LB/RB/RT, BACK, START e D-pad. Ele usa os bindings padrão de teclado do port. Não lê nem altera os mapeamentos ou posições personalizados da luta.
- Luta: mantém VirtualControlsView, os sprites, o D-pad agrupado, editor e salvamento v7/v8.
- AUTO: consulta o módulo publicado por um hook já existente no contexto guest. O módulo 2 (game) usa o pad de luta; 3/6 (tutorial/gym no mapeamento existente, ainda com nomes inferidos) também. Front end, cinema, loaders ou tipo desconhecido usam Xbox. Quando o overlay do port está aberto, o toque tem prioridade. Não há leitura de RAM guest a partir da UI Android.
- PAD: AUTO / XBOX / LUTA permite corrigir manualmente um contexto não reconhecido, sem remapear os botões da luta. A seleção manual vale para a sessão; o menu do port continua recebendo toque.
- HUD: mostra FPS, CPUs efetivamente aplicadas/pedidas e teto de FPS escolhido. CPUs permitidas não significam uso simultâneo de todas.
- CPU: a thread guest principal é mantida no núcleo preferido pela política anterior (maior ID permitido). As workers podem usar o subconjunto configurado de 1..8. Isso evita que a thread principal migre entre CPUs de IDs menores ao aumentar a contagem; não é um agendador baseado em topologia e não paraleliza a simulação.
- O kernel registra a máscara realmente aplicada e o resultado do scheduler. O Android pode disponibilizar menos CPUs que o pedido. Nenhuma restrição do sistema é removida.
- A cada cerca de 2 s, [ANDROID-CPU] registra tempo de CPU do processo dividido pelo tempo real, junto da máscara e do cap. Inclui renderer e áudio: não mede isoladamente a thread guest nem comprova ganho. Mantém espera normal no limitador e não faz busy-spin.
- FPS de 30..165 continua no launcher. A preferência salva do usuário não é alterada. Para comparar CPUs, escolher 60 ou mais e repetir a mesma luta; um cap de 30 oculta ganhos acima de 30. Mais CPUs ainda podem reintroduzir a corrida da intro documentada no runtime.

## Validação nesta rodada

- 4 testes passaram: afinidade real no host com máscara escolhida e thread principal em uma CPU; pacing 30..165 e estados da bridge JNI com stubs; geometria Java do D-pad; adaptador de toque SDL de produção compilado com a biblioteca Dear ImGui vendorizada REAL e executado para conferir coordenadas, down/up/cancel e mouse existente.
- O teste de estado cobre o módulo game/tutorial/gym, menus, prioridade do overlay e publicação da quantidade efetiva de CPUs. Não valida em qual módulo cada tela real do jogo entra.
- Os 8 arquivos Java do app passaram pelo parser javac 17, sintaxe somente.
- kernel_bridge.c passou na checagem sintática GCC host com __ANDROID__ definido: 0 erros e 30 avisos de código anterior. Não foi uma compilação NDK.
- Não há build APK/Gradle/NDK neste ambiente. Não houve teste Android por toque, teste da troca automática em luta nem benchmark em aparelho. Não declarar os problemas de desempenho corrigidos sem a comparação abaixo.
- Nenhum lock do guest, timestep, shader, função recompilada ou arquivo comercial foi alterado/distribuído.

## Instalação e teste

O pacote é cumulativo sobre v7 OU v8: conserva o slider 30..165 e D-pad agrupado mesmo se o usuário ainda estiver no v7. O instalador verifica todos os arquivos antes de escrever e guarda backup. Não aplique instaladores anteriores depois deste.

1. Compilar Release, instalar e manter 1 CPU / 60 FPS para o primeiro teste.
2. Em Press Start/menu, confirmar os botões Xbox. Em uma luta, conferir a arte personalizada e a preservação do layout. Se AUTO escolher errado, usar PAD: XBOX ou PAD: LUTA e registrar a tela.
3. Abrir MENU, tocar em abas/opções/sliders, arrastar a barra de rolagem e fechar. Repetir para conferir que não ficou um clique preso nem uma tecla do jogo pressionada.
4. Usar CONTROLES durante uma luta pausada, salvar/cancelar e voltar; conferir D-pad agrupado e multitouch. Repetir segundo plano/retomada.
5. Repetir os mesmos 60 s de luta com 1 CPU e depois 5 CPUs, sempre no mesmo cap de pelo menos 60 e reiniciando entre opções de CPU. Enviar o log de cada teste; comparar FPS, [VKPERF], [ANDROID-CPU] e o ritmo/áudio.
6. Se houver falha de compilação, enviar o erro; se houver fechamento Android, enviar também logcat. O fragmento "problema que mais de" não descreve um erro específico e não é suficiente para afirmar corrigido um crash acima de certa contagem.
