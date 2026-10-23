# v16 — fila Vulkan assíncrona e CORRER/ESPECIAL

## O que mudou

CORRER agora envia B (tecla L); ESPECIAL envia LT (tecla Q). Corrige somente essas duas funções
uma vez no controle salvo, sem reposicionar botões. SOCO=Y, CHUTE=X, AGARRAR=A e DEFENDER=RT são
mantidos. O editor continua permitindo novas associações; Padrão usa o mapa corrigido.

No Android, os envios normais à GPU deixam de esperar imediatamente no Present. Dois contextos
alternam, e a CPU só espera quando precisa reutilizar um contexto ainda ocupado. Isso permite
preparar comandos enquanto a GPU executa o envio anterior. Não gera quadros artificiais.

O princípio é o mesmo usado por Begin/Submit/ResetFrame/Present do Black:
https://github.com/RanggaCasper/Black-Android/blob/main/native/vulkan.cpp
O trecho atual1740–1860 foi consultado novamente; blob a1cc885f64fd883dfbca26760aac7b0e60561d62.
Black usa3 contextos e outro tradutor. Aqui a implementação C é própria, com2 contextos e o tradutor
NV2A/shaders existente do Def Jam. Não foi transplantado o backend inteiro nem código do TK3.

## Como a memória e a sincronização são protegidas

- Cada contexto tem seus próprios command/descriptor pools, command buffers, fence e semáforo de
  aquisição. Pool e faixas só são reutilizados depois de a fence daquele contexto terminar.
- Os132MiB de buffers mapeados existentes são divididos em duas faixas separadas. Não duplica essa
  alocação grande. Há o custo adicional dos pequenos objetos de comando/descritores/sincronização.
- O semáforo de fim de renderização pertence à imagem da swapchain, não ao número do frame CPU.
  Present espera esse semáforo. A recriação da superfície drena a fila antes de liberar objetos.
- Texturas retiradas ficam guardadas até o número do envio que poderia usá-las ter terminado.
  Readback usa números de envios enfileirados e consulta fences reais antes de entregar pixels.
- Capturas que precisam dos pixels agora continuam síncronas. Atualização da gamma espera leitores
  anteriores. Um upload maior que a meia faixa drena os contextos, usa a capacidade original inteira
  e termina aquele frame sincronamente. Uma reserva conjunta evita flush no meio dos dados de um draw.
- Dependências explícitas ordenam os acessos aos alvos color/depth/gamma compartilhados na mesma fila.
  Se o segundo contexto não puder ser criado, usa o fluxo síncrono. Falha de semáforo de apresentação
  também faz a apresentação esperar a GPU, evitando esperar um sinal que não será produzido.

## O que o log mostra

O log enviado junto desta solicitação ainda mostra aproximadamente20–21FPS, com quedas para14–19.
[PERF] tem49,72/55,85ms de frame,5,00/5,51ms de coleta e6,26/7,37ms de envio de desenhos. A espera de
CPU pela fence GPU fica perto de12–14ms. Pequenas mudanças anteriores não resolveram o desempenho.
Esses números não provam que todo o restante seja código do jogo, nem identificam a causa das quedas.
A nova fila permite sobrepor trabalho; não existe ganho de FPS confirmado no aparelho ainda.

## Como usar e comparar

Renderização assíncrona fica ligada por padrão em Configurações, no início do app. Para comparar com
o fluxo anterior, desligue e reinicie o jogo. A opção não muda resolução, núcleos, limite de FPS ou
velocidade da luta. O teto165 permanece um teto, sem desbloqueio real de120/144/165 nesta versão.

O APK mostra versão0.5.2-v16-async (versionCode16). No início do defjam-native.log devem aparecer:

    [ANDROID] async Vulkan queue 1 (v16)
    [D3D8-VK] render queue: 2 context(s), disjoint mapped ring ranges; async enabled

[VKQUEUE] informa envios async/sync, fences ocupadas e completed/queued. [VKPERF] GPU fence mede o
TEMPO QUE A CPU ESPERA, não a duração física dos desenhos na GPU. Valor menor após esta mudança
não significa, sozinho, GPU mais rápida ou maior FPS. Compare a taxa real [D3D] e os tempos [PERF].

Faça60segundos da mesma luta com os mesmos ajustes, primeiro ligado e depois desligado. Compare em
condições semelhantes de temperatura. Envie defjam-native.log novo. Verifique também sair/voltar
ao app, menu Port touch, capturas e os botões da luta. Não limpe dados nem desinstale o aplicativo.

## O que foi testado aqui

8 fixtures host passaram: afinidade1–8, pacing/contexto de entrada, geometria Dpad, cache de descritores,
estado dinâmico, esperas GPU, saída do gather/pool e a nova fila. O teste da fila compila o backend
completo e executa os caminhos reais contra chamadas Vulkan simuladas: não reutilizar recursos em voo,
faixas separadas, descarte adiado, readback, overflow, uploads grandes, falhas de submit/wait, falhas
parciais de criação do contexto, semáforos por imagem e desligamento da opção. ASan/UBSan usados;
a inscrição global do ASan é desativada nesse fixture para permitir remover APIs não ligadas ao teste.

O fonte Vulkan completo passou sintaxe host pelos caminhos Android e Linux;8 arquivos Java passaram
no parser17, sem resolver Android SDK. Não há SDK/NDK, dados do jogo, código guest gerado ou ICD Vulkan
funcional aqui. Não foi compilado APK, executado teste de GPU real, gameplay ou benchmark no telefone.
As dependências Vulkan, a imagem e o retorno do segundo plano ainda precisam de aceitação no aparelho.

Aplicar-v16.ps1 valida os arquivos antes de escrever, faz backup e compila localmente com -Build.
Somente depois de BUILD SUCCESSFUL instale por adb install -r. Se o build falhar, envie o primeiro erro
C/C++ e o final do Gradle; não instale APK antigo. A coleta privada de fontes de tempo é mantida para
investigar separadamente o desbloqueio de FPS real acima de60. O ZIP não contém jogo nem APK.
