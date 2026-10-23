# Android v11 — preparação de vértices em paralelo

Aplicar sobre v9 ou v10. Preserva START, controles Xbox nos menus, controles personalizados na luta,
editor, D-pad agrupado, menu do port por toque e FPS30..165. Inclui a retirada da fixação exclusiva
da thread principal do v9.

## Uso

No launcher, habilite **Preparar vértices em paralelo • experimental** e selecione5 núcleos.
Reinicie o jogo para aplicar. Para comparar, mantenha a mesma luta, resolução e cap de60FPS ou mais.
Faça60s com a opção desligada e60s ligada, salvando o log de cada execução.

## O que divide

Somente coleta/conversão de atributos dos lotes de vértices enviados ao caminho GPU, com pelo
menos2048 entradas distintas segundo o cache de índices existente. Cria até7 auxiliares persistentes,
mais o executor. A contagem inclui o executor e respeita o pedido e a afinidade permitida ao executor.
Cada tarefa escreve uma faixa separada do buffer compacto. O executor espera todas antes de enviar
à GPU. A lógica do jogo, programa de vértices em CPU e comandos Vulkan continuam em sua ordem atual.
Lotes menores ficam em uma thread; falha ao criar auxiliares reduz a quantidade ou usa uma thread.
Não cria tarefas apenas para elevar o uso de CPU. Pode não acelerar, especialmente se os lotes forem
pequenos ou o gargalo estiver na lógica da luta; nenhum ganho em aparelho foi medido aqui.

Também elimina a cópia que antes compactava cada vértice após preencher um buffer com stride de64floats.
A opção desligada usa a preparação compacta em uma thread para comparar o efeito das auxiliares.

## Evidência e testes

`[ANDROID-VERTEX] requested...` informa configuração. A linha com `participants, ... batches / ... vertices`
só aparece após executar um lote nas tarefas auxiliares. Esses participantes não significam núcleos
ocupados por todo o frame. `[PERF] gather+pack` mede o tempo dessa etapa, incluindo sincronização.

Fixture host compila o executor/decoder/pool reais e compara bytes da saída com o caminho serial
anterior: máscaras vazias/esparsas/completas, formatos, índices repetidos, atributos ausentes e fronteiras
de tamanho. Testa150 despachos consecutivos, escrita única por faixa, execução em threads distintas,
falhas parciais/totais de criação, parada/reinício e buffer truncado. ASan/UBSan passaram; o detector
de vazamentos foi desligado por limitação de acesso a `/proc` neste ambiente. Outros4 fixtures de
controle/afinidade/pacing/toque passaram. Java: parse de sintaxe; executor C: sintaxe Android e Linux.
Sem compilação de APK, teste de gameplay ou medição de FPS em aparelho.

## Aplicação e retorno

`Aplicar-v11.ps1 -ProjectRoot <pasta> -Build` valida hashes, faz backup em `logs/v11-backups`, copia
apenas fontes/documentos/testes e atualiza datas para a compilação não reutilizar objetos antigos.
Pode repetir a aplicação. Se houver arquivo desconhecido, para antes de alterar fontes.
Para desligar as auxiliares, desative a opção e reinicie. Para desfazer fontes, restaure os caminhos
salvos no backup; não apague saves ou arquivos do jogo.
