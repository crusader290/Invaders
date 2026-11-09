# ROMs

`invaders.zip` is the MAME *invaders* set (Taito/Midway, 1978), taken from
[cbeust/space-invade.rs](https://github.com/cbeust/space-invade.rs/tree/3cecc86da877fb65c55779906aae6219e3b26361/roms).
It holds four 2 KB chips, which CMake joins in the order h, g, f, e to make the
8 KB program image at configure time:

| chip | address |
| --- | --- |
| invaders.h | 0000-07FF |
| invaders.g | 0800-0FFF |
| invaders.f | 1000-17FF |
| invaders.e | 1800-1FFF |

SHA-256 of the assembled image:
`7446e0994117596de5206519e693f8875ff3455e0be121d5cb975c3bcc224c4e`

To use a different image, put it here as `space-invaders.rom` (ignored by git) or
configure with `-DSI_ROM=/path/to/file`.

The WAV samples the game plays are in [`../sounds`](../sounds), from the same
upstream repository.
