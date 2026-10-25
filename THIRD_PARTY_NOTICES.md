# Attribution and distribution

The GPL-derived application and combined executable are distributed under GPL-2.0-only; see [LICENSE](LICENSE). Preserve individual file notices. Some local files carry MIT notices, and EDK2 headers retain BSD-2-Clause-Patent notices.

- Primary adaptation reference: [TOTORONG/AD103_win_unlocker](https://huggingface.co/TOTORONG/AD103_win_unlocker), revision `51df9c17ec89eb8486df52cc9079dde4bd1f9974`.
- Unlock-core ancestry: [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock/tree/0ebdb1c6308b6a2072be5bf04513472942541956), GPL-2.0-only; see preserved [upstream notices](UPSTREAM-THIRD_PARTY_NOTICES.md).
- Diagnostic reference: [JustAResearcher/gpu-unlock-ada](https://github.com/JustAResearcher/gpu-unlock-ada), revision `4e7fefcc4923f9e083d5324dc75a7d2f906af77f`.
- EDK2 headers: [TianoCore EDK2](https://github.com/tianocore/edk2/tree/c1415771ac592ad8509351c46cd3cda4f38d7d52), preserved header hashes in `vendor/edk2/headers.json`.
- Further NVIDIA and CMP40HX notices are retained under `LICENSES/`.

Local changes adapt the measured 4080 SUPER 16GB board, UEFI ownership/PCI/DMA handling, outcome validation, diagnostics and one exact Device Status transition. The existing V67 payload is upstream work; this repository does not claim its exploit discovery or binary authorship.

This repository includes the locally built EFI and its corresponding C source, test source, headers, build tooling and original tested build script/manifest. The portable top-level build tools differ from the archived build tooling; rebuilt binaries are not asserted byte-identical or separately hardware tested.

Raw ROM, PROM-bearing captures and firmware payload binaries are not committed. The asset tool obtains unchanged hash-pinned files directly from upstream. Their licenses/terms are separate from this repository's application license; presence of a download URL is not a grant of redistribution rights. The upstream narrative README is not copied. Newly written local documentation is supplied with this repository under GPL-2.0-only to the extent of the publisher's rights; third-party material retains its own terms.
