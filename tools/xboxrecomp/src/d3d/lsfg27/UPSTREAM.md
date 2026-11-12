# Procedência

Backend compute derivado de **lsfg-vk-android/framegen**, licença MIT, no commit
`f5bf8858d3318e03df06be863bfd53f6b885e899` do repositório
https://github.com/amyroseee/LSFG-Android-A6xx .
Projeto de origem: https://github.com/FrankBarretta/lsfg-vk-android .
Os avisos de copyright e a permissão estão em `vendor/LSFG-LICENSE.md`.
Volk é o snapshot presente nesse mesmo commit; licença em `vendor/volk/LICENSE.md`.

Somente o backend MIT e Volk são incluídos. Não há código do aplicativo Android,
MediaProjection, overlay, NNAPI ou UI de terceiros. Não há DLL ou shader extraído
de Lossless Scaling no pacote. O usuário importa sua própria DLL.

Adaptações v27:

- `borrowed27.cpp` substitui a criação de instância/dispositivo por handles
  emprestados do jogo e registra a cadeia Performance no command buffer do
  apresentador. Não faz submits privados nem inicia outro driver.
- A tabela Volk é privada, oculta e vinculada com `-Bsymbolic`; recebe o
  `vkGetInstanceProcAddr` do Turnip que o jogo já escolheu.
- `image27.cpp` cria apenas imagens privadas, mantém dependências view/image/memory
  por RAII e limita cada dimensão dos níveis pequenos a pelo menos1.
- `core/device.hpp` e `core/commandbuffer.hpp` recebem os construtores emprestados.
- `common/utils` usa armazenamento fixo para as barreiras por dispatch, com
  queue family ignorada e dependências que incluem cópias/leituras anteriores.
- `buffer.cpp` exige memória simultaneamente host-visible e coerente.
- Os destruidores liberam também os pequenos wrappers de handles.
- `exception.cpp` usa concatenação de strings para evitar dependência de `std::format`.
- Mipmaps/alpha/beta/gamma/delta/generate preservam a cadeia compute original.
  Históricos são inicializados antes da primeira leitura e recomeçam após lacunas.

O importador Python lê recursos PE, usa as variantes SPIR-V prontas FP32 (+98)
e FP16 (+49), e reorganiza bindings por tipo/slot para a interface do backend.
Não executa `LoadLibrary` nem emula a DLL x86-64.

Adaptações v30:

- ABI2 exige multiplicador 2/3/4 e fase explícita no record; misturas de ABI são recusadas.
- generationCount=M-1 usa as frações já previstas pela cadeia Performance.
- NN/optical-flow compartilhados por par; gamma/delta/generate calculados por fase solicitada.
- Uma imagem privada por fase e cache de fases já calculadas/apresentadas.
- Testes sintéticos FP32 em Vulkan de software não validam qualidade ou desempenho Android.
