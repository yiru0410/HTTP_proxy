cc = gcc
prom = HTTP_proxy
deps = $(shell find . -name "*.h")
src = $(shell find . -name "*.c")
obj = $(src:%.c=%.o)

$(prom): $(obj)
	$(cc) -pthread -o $(prom) $(obj)

%.o: %.c
	$(cc) -c $< -o $@

clean:
	rm -rf $(obj) $(prom)
