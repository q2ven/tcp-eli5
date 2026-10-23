# Writing guide for tcp-eli5

This file is for contributors (human or AI).  Readers can skip it.

## Target source tree

- Everything in this book describes **upstream net-next only**.
- Pinned commit: `279d5c8afcd38bd841832a39bfe1aa4f8518d98e`
  (`net: dsa: motorcomm: Extend ACL with UDF`, 2026-10-21).
- Do not describe out-of-tree, vendor or Google-internal (prodkernel)
  behaviour, and do not mention internal links, benchmarks or bugs.

## Source links

- Base URL:
  `https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/tree/<path>?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e#n<LINE>`
- Put the link on the **file path**, not on the function name.
  Function / struct names are always written in backticks with `()`
  for functions:

  ```markdown
  `tcp_sendmsg()`（[net/ipv4/tcp.c#L1386](https://git.kernel.org/...#n1386)）
  ```

- `<LINE>` must be the line where the function / struct definition
  starts in the pinned tree (verify with `grep -n`).

## Chapter template

Every chapter has the same shape:

1. **たとえ話で言うと / The ELI5 version** – a short story using the
   shared metaphors below.  No code.
2. **RFC では / What the RFCs say** – start with a plain one-paragraph
   summary of each relevant RFC (number, title, what problem it
   solves), then go into the details that matter for the code: rules,
   state variables, formulas, pseudo-code.  Cite sections as
   `[RFC 9293 §3.8.6](https://www.rfc-editor.org/rfc/rfc9293#section-3.8.6)`.
   Mention when an RFC is obsoleted/updated and which one Linux follows.
3. **全体の流れ / The big picture** – one diagram (mermaid
   `flowchart` or `sequenceDiagram`) plus a few sentences.
4. **コードを追う / Walking through the code** – `### Step 1: ...`,
   `### Step 2: ...`.  For the key functions of the chapter, walk the
   function **line by line**: quote it verbatim from the pinned tree in
   consecutive chunks (each chunk <= ~40 lines, nothing important
   skipped) and after each chunk explain what every non-trivial line
   does and why, tying it back to the RFC rule it implements.  Less
   central helpers may be trimmed with `...`.
5. **RFC と実装の違い / RFC vs Linux** – a table of places where Linux
   deviates from, extends, or predates the RFC text (with reasons).
6. **設計の背景と歴史 / Design history** – the key upstream commits
   that shaped the code in this chapter (short sha, subject, author,
   one or two sentences on *why* it was done and what problem it
   fixed), found with `git log` / `git log -L :func:file` in the
   pinned tree.  Focus on the reasoning maintainers used.
7. **性能の勘所 / Performance notes** – why the code is written the
   way it is: cacheline groups in `tcp_sock`, `likely()/unlikely()`,
   lockless readers with `READ_ONCE()/WRITE_ONCE()`, avoiding
   divisions, fixed-point arithmetic, batching, prefetching, and
   which socket lock / softirq context each step runs in.
8. **観測とデバッグ / Observing it** – relevant MIB counters (`nstat`
   names), `tcp_info` fields (`ss -ti`), tracepoints, sysctls, and
   packetdrill tests under `tools/testing/selftests/net/packetdrill/`
   that exercise this code.
9. **IPv4 と IPv6 / IPv4 vs IPv6** – IPv4 and IPv6 are given equal
   weight.  Whenever the paths differ, show both entry points side by
   side (a table works well) and explain where they converge.  If the
   chapter's code is address-family independent, say so in one line.
10. **理解度チェック / Check your understanding** – 3-6
    maintainer-level questions (e.g. "what breaks if this line is
    removed?", "why is this `READ_ONCE()` needed?"), each answer in a
    `<details><summary>答え</summary>...</details>` block.
11. **まとめ / Summary** – bullet list, plus "よくある誤解 / Common
    pitfalls" when useful.
12. **次の章 / Next** – one line pointing to the next chapter.

## Audience

The reader is an experienced Linux networking contributor who knows
the socket / lookup / netns / control-plane ("slow path") side well
but wants to master the data-transfer fast path and congestion
control at the level of the TCP maintainers.  So: the ELI5 part stays
short and friendly, but the code walk must be maintainer-grade –
invariants, corner cases, races, and the *why* behind each line.

Depth: the data-transfer fast path (chapters 3-8) and congestion
control (chapters 11-14) are the heart of the book and should be the
most detailed.  Connection setup / teardown is intentionally brief.

## Shared metaphors (use consistently across chapters)

| Kernel concept | Japanese | English |
| --- | --- | --- |
| host / IP address | 建物の住所 | building address |
| port | 部屋番号 | room number |
| socket (`struct sock`) | 部屋の郵便受け | the room's mailbox |
| `sk_buff` | 伝票付きの小包（ヘッダ = 伝票） | parcel with a shipping label (headers = label) |
| listening socket | 受付窓口 | reception desk |
| `request_sock` | 仮の整理券 | temporary ticket |
| SYN cookie | 控えを取らない割符 | tally stick with no stub kept |
| accept queue | 待合室 | waiting room |
| write queue / send buffer | 発送待ちの棚 | outgoing shelf |
| receive queue | 受け取り箱 | inbox |
| out-of-order queue | 順番待ちの棚 | holding shelf |
| ACK | 受領印 | signed receipt |
| retransmission | 再発送 | re-shipping |
| cwnd | 一度に道路へ出せるトラックの台数 | trucks allowed on the road at once |
| timers | キッチンタイマー | kitchen timers |
| TIME_WAIT | 退去後しばらく部屋番号を貸さない期間 | cooling-off period before reusing a room number |
| memory accounting | 家計簿と予算 | household budget |

## Language rules

- `ja/` and `en/` have the same files and the same structure.
- Japanese: です・ます調.  Keep kernel identifiers, struct fields and
  RFC terms in English.
- English: plain, friendly, British spelling.
- Use `-` for bullet lists.
