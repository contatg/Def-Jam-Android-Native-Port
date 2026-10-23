# Def Jam Android v7 — continuação dos fontes enviados em 22/10/2026

## O que mudou

- START separado e sempre disponível na barra superior esquerda, mesmo se o controle touch estiver desligado, oculto ou remapeado. Envia Enter (binding padrão do port), solta após 120 ms e ao perder foco/pausar. O contador de FPS foi deslocado para a direita.
- CONTROLES ou o desenho de chave abre o editor como uma View dentro de DefJamActivity. A superfície SDL/Vulkan permanece na mesma Activity. Salvar recarrega o controle; Cancelar/Voltar descarta as alterações. O jogo continua ao fundo: pause com START antes de editar. Escolher permite selecionar controles escondidos pelo painel.
- A arte de luta e os mapeamentos existentes foram preservados. As setas agora movem usando WASD (stick esquerdo do port). Na migração para v7, os dois analógicos ficam ocultos e as setas visíveis; depois de Salvar, a escolha pessoal de visibilidade é preservada. É possível reativar os analógicos no editor. O layout passa a ser lido do arquivo privado controls-v7.json a cada abertura do controle, com escrita via AtomicFile. O launcher e o processo :game não sobrescrevem os layouts um do outro por cache de SharedPreferences; os c6_* antigos são importados quando ainda não há arquivo v7. Editores simultâneos não são suportados: a última gravação vence. O analógico direito continua sendo a entrada digital de setas do teclado usada para Blazin'.
- Launcher > Desempenho: escolha de 1 a 8 CPUs permitidas às threads guest. Aplicada ao iniciar o jogo; não cria threads adicionais e não garante ganho. Se o Android permitir menos CPUs que o pedido, usa somente as disponíveis. Mantém a preferência anterior pelos IDs mais altos, sem afirmar que isso identifica a topologia mais rápida de todo aparelho. Padrão: 1. Mais de 1 pode reintroduzir a corrida na intro já documentada no runtime.
- Launcher > Desempenho: limite original ou 30 FPS experimental. Durante o jogo, MENU permite trocar o limite sem reiniciar, somente para a sessão atual; a escolha salva para as próximas sessões fica no launcher. Limita a frequência de apresentações com espera no executor gráfico, sem busy-spin. NÃO altera delta-time do jogo. Pode reduzir o ritmo quando a simulação aguarda o executor: conferir animação, áudio e relógio em luta. Não foi demonstrado ganho de desempenho.
- A preferência explícita de CPUs do launcher prevalece sobre o antigo multicore.txt. O arquivo legado permanece utilizável quando o programa é iniciado sem o argumento de CPUs.
- As alterações anteriores de fullscreen, drivers Turnip e recuperação de superfície do v6 foram mantidas.

## Aplicação e compilação

Este é um patch para o ZIP DefJam-Fontes-20261022-103303.zip enviado pelo usuário.
Não execute novamente o instalador v6 depois deste patch: ele substitui as classes novas.

Execute Aplicar-v7.ps1 com -ProjectRoot apontando para a pasta atual do projeto.
O script verifica TODOS os hashes antes de modificar arquivos, faz backup e tenta reverter se uma cópia falhar. Uma mudança local desconhecida faz o script abortar, para não sobrescrever trabalho posterior. Arquivos já atualizados são reconhecidos.

Use -Build -DataDir "C:\Games\DefJamFFNY-data" -Variant Release para chamar o compilador já existente no projeto. O script não limpa caches nem força nova recompilação do código guest. Os objetos nativos afetados serão recompilados incrementalmente.

## Verificações efetivamente feitas neste ambiente

- 2 testes nativos passaram com GCC C11, -Wall -Wextra -Werror: seleção de máscaras esparsas, opções 1..8/all, valores inválidos, limitação à disponibilidade e aplicação REAL da função de afinidade extraída do kernel no Linux. A sincronização de inicialização usa shims de Interlocked e não foi testada sob concorrência Android.
- Bridge de pacing de produção compilado e exercitado com tipos JNI mínimos e relógio/sono controlados: primeiro frame, intervalo de 33,333 ms, frame lento, retorno após 5 s, interrupção EINTR e ativação/desativação ao vivo. Esses doubles não validam o driver nem a execução em ARM64.
- kernel_bridge.c inteiro passou na análise sintática GCC com __ANDROID__ definido, usando os headers do projeto; houve avisos de código já existente. Não é uma compilação NDK.
- Os 5 arquivos Java do app passaram pelo parser oficial javac 17. Não houve resolução de tipos Android nem empacotamento Gradle.
- 6 fixtures de entrada existentes foram puladas por exigirem MSVC/Windows.
- NÃO há APK compilado neste ambiente: Android SDK/NDK e Gradle não estão instalados, e o ZIP de fontes não inclui o código guest gerado.
- NÃO houve teste em celular, gameplay, teste da interface por toque ou validação do mapeamento SOCO/CHUTE/etc. O comportamento em luta e a retomada do v6 ainda precisam ser testados no aparelho.
- O instalador PowerShell foi revisado e os hashes do pacote conferidos; não foi executado em Windows neste ambiente.

## Teste no aparelho / próximo passo do Claude

1. Compilar Release no PC, instalar com adb install -r android\app\build\outputs\apk\release\app-release.apk.
2. Manter 1 CPU e limite original inicialmente. Conferir START na tela inicial e pausa/retomada em luta; verificar que o FPS não o cobre.
3. Abrir CONTROLES com uma luta pausada: mover, redimensionar, remapear, ocultar, Salvar; reabrir e Cancelar uma alteração; conferir persistência ao reiniciar o app. Conferir setas para movimento/menu. Testar multi-toque, foco e segundo plano.
4. Ativar 30 FPS em MENU e comparar 60 s do relógio real com o ritmo/áudio da mesma luta no modo original. Se houver câmera lenta, voltar ao original e investigar frame skipping/timestep separadamente.
5. Testar 2..8 CPUs separadamente, sempre reiniciando o jogo. Conferir no defjam-native.log a linha Android guest affinity e a quantidade efetivamente permitida. Se travar na intro, retornar a 1. Não usar contagem de núcleos como prova de aceleração.
6. Voltar do segundo plano, conferir imagem, controles e fullscreen. Enviar o log completo de 60 s de luta e qualquer erro de compilação; não inferir desempenho a partir de menu ou APK gerado.

Nenhum dado comercial do jogo, ISO, XBE, código guest gerado ou APK integra este patch. Nada foi enviado ao GitHub.
