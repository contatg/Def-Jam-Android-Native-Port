# Def Jam FFNY Android ARM64

Port Android experimental de Def Jam: Fight for NY, baseado em
[vyanhursky/DefJamFFNY-recomp](https://github.com/vyanhursky/DefJamFFNY-recomp) e seu runtime
xboxrecomp. Licenças e avisos dos componentes são preservados no projeto.

O runtime local está incluído em tools/xboxrecomp. Não é necessário inicializar submódulo nesse
snapshot. SDL/ImGui/shaderc são preparados pelo script Android; volk/adrenotools são fontes locais.
Nenhuma ISO, XBE, save, log, código guest gerado, SDK ou biblioteca compilada entra neste repositório.

## Compilar no PC

Use seu próprio dump USA verificado e siga [android/README.md](android/README.md).
Depois de compilar, instale com:
