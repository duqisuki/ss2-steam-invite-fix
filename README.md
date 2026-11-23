> **English:** Serious Sam 2 (Steam, patch 2.200+) ignores accepted Steam invites: clicking
> "Accept" / "Join Game" does nothing. This mod is a tiny `steam_api.dll` proxy written in C
> (built with Zig) that forwards every call to the original DLL and additionally handles
> `GameLobbyJoinRequested_t`, `GameRichPresenceJoinRequested_t` and `+connect_lobby`, joins the
> host's lobby and starts the game's own client connection. One-line install (both players,
> game closed):
>
> ```powershell
> irm https://raw.githubusercontent.com/duqisuki/ss2-steam-invite-fix/main/install.ps1 | iex
> ```
>
> Tech: C · Win32 · Steamworks API · DLL proxying · Zig toolchain · PowerShell. License: [MIT](LICENSE).

---

# Serious Sam 2 — фикс приглашений Steam

В Serious Sam 2 (Steam, патч 2.200+) сетевая игра идёт через Steam-лобби, но игра
**не обрабатывает принятие приглашения**: друг жмёт «Принять» или «Присоединиться к игре», и ничего не происходит.
Этот мод добавляет недостающую обработку.

## Установка (обоим игрокам)

Закройте игру, откройте PowerShell и выполните:

```powershell
irm https://raw.githubusercontent.com/duqisuki/ss2-steam-invite-fix/main/install.ps1 | iex
```

Установщик сам найдёт игру в любой библиотеке Steam. Если не нашёл:

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/duqisuki/ss2-steam-invite-fix/main/install.ps1))) -GamePath "D:\SteamLibrary\steamapps\common\Serious Sam 2"
```

Удаление:

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/duqisuki/ss2-steam-invite-fix/main/install.ps1))) -Uninstall
```

После обновления или проверки файлов игры в Steam запустите установку ещё раз.

## Как играть

1. Хост: «Мультиплеер» → создать **онлайн**-игру (не LAN), загрузиться на карту.
2. Хост: Shift+Tab → друзья → ПКМ по другу → «Пригласить в игру»
   (или друг просто жмёт «Присоединиться к игре» в списке друзей Steam).
3. Друг принимает приглашение и автоматически подключается к игре. Если игра у друга была закрыта,
   Steam запустит её, и примерно через 12 секунд после появления меню начнётся подключение.

Если на сервере стоит пароль, у друга должен совпадать `net_strConnectPassword`.

## Как это работает

`steam_api.dll` заменяется маленьким прокси, а оригинал сохраняется как `steam_api_orig.dll`.
Все вызовы прокси передаёт оригиналу и дополнительно:

- ловит `GameLobbyJoinRequested_t`, `GameRichPresenceJoinRequested_t` и аргумент запуска `+connect_lobby <id>`;
- входит в лобби хоста (`ISteamMatchmaking::JoinLobby`) и ждёт `LobbyEnter_t`;
- выполняет встроенную консольную команду игры `sam_strServerName = "<lobby>"; samStartClient();`,
  то есть тот же путь подключения, что и у браузера серверов.

Лог: `Bin\ss2_invite_fix.log`.

## Сборка

Нужен [Zig](https://ziglang.org) 0.13:

```powershell
.\build.ps1 -Zig C:\path\to\zig.exe
```
