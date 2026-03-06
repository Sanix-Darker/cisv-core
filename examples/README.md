# Core Examples

Build and run:

```bash
make -C core all
cc -Icore/include examples/basic_parse.c -Lcore/build -lcisv -o /tmp/cisv_basic
LD_LIBRARY_PATH=core/build /tmp/cisv_basic

cc -Icore/include examples/iterator_example.c -Lcore/build -lcisv -o /tmp/cisv_iter
LD_LIBRARY_PATH=core/build /tmp/cisv_iter
```
