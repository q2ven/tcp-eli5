# tcp-eli5

A step-by-step, "explain like I'm five" tour of the Linux kernel TCP
stack, based on **upstream net-next** at commit
[`279d5c8afcd3`](https://git.kernel.org/pub/scm/linux/kernel/git/netdev/net-next.git/commit/?id=279d5c8afcd38bd841832a39bfe1aa4f8518d98e).

Linux カーネルの TCP スタックを、たとえ話とコードの両方から一歩ずつ
たどる解説書です。

- 日本語版: [ja/](ja/README.md)（BPF CC / Qdisc ハンズオン: [labs/README.md](labs/README.md)）
- English: [en/](en/README.md) (BPF CC & Qdisc Hands-on Labs: [labs/README.en.md](labs/README.en.md))

## GitBook

The repository is laid out for GitBook site-level Git Sync:
`gitbook-docs.yaml` at the root maps the `ja` and `en` directories to
two spaces, and each directory has its own `.gitbook.yaml`,
`README.md` and `SUMMARY.md`.  Keep the project directory at the
repository root.  Diagrams use mermaid code blocks, so enable the
Mermaid integration in GitBook.

## Contributing

See [STYLE.md](STYLE.md).
