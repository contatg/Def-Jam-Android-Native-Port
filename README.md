# Def Jam FFNY Android ARM64

Port Android experimental de Def Jam: Fight for NY, baseado em
[vyanhursky/DefJamFFNY-recomp](https://github.com/vyanhursky/DefJamFFNY-recomp) e seu runtime
xboxrecomp. Licenças e avisos dos componentes são preservados no projeto.

Versão de fontes: **0.5.5-v19-packets**. Controles personalizados, editor pela chave no menu/luta,
contador somente FPS, guest1 e CPUs nativas Auto. Veja [mudanças/testes](docs/android-v19-pt-BR.md).

O runtime local está incluído em tools/xboxrecomp. Não é necessário inicializar submódulo nesse
snapshot. SDL/ImGui/shaderc são preparados pelo script Android; volk/adrenotools são fontes locais.
Nenhuma ISO, XBE, save, log, código guest gerado, SDK ou biblioteca compilada entra neste repositório.

## Compilar no PC

Use seu próprio dump USA verificado e siga [android/README.md](android/README.md).
Depois de compilar, instale com:

```powershell
& "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe" install -r "android\app\build\outputs\apk\release\app-release.apk"
```

## GitHub Actions

Android ARM64 executa os fixtures host a cada push. O build APK completo é manual e privado:
configure os secrets DEFJAM_GUEST_SOURCES_URL e DEFJAM_GUEST_SOURCES_SHA256 com um ZIP HTTPS
privado dos .c/.h de src/recomp/gen já gerados no seu PC. Depois use Run workflow com apk ligado.
Sem esses arquivos o build para antes da compilação. Não publique esses inputs no Git.

Não há ganho de FPS v19 confirmado em aparelho. Testes de código e compilação não comprovam
menu, luta ou retorno do segundo plano. O APK gerado precisa de validação no celular.
