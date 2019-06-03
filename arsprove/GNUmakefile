#CGI_URL=http://10.0.0.4/private/public/65test.cgi
CGI_URL=http://localhost/~sbizna/65test.cgi

all: test

include generated_tests.mk

# do not regenerate all tests automatically when gen_irqphase_tests is updated
# YET. (TODO: merge gen_irqphase_tests and gentests later)
generated_tests.mk: util/gentests.lua gen/optable.csv $(wildcard util/am_tmpls/*.65c)
	rm -rf tests/gen
	@mkdir -p tests/gen/irqphase
	util/gen_irqphase_tests.lua
	util/gentests.lua

bin/arscore: obj/arscore.o
	@mkdir -p bin
	c++ -std=c++14 -Wall -Werror -O3 -ljsoncpp $^ -o $@

obj/%.o: src/%.cc
	@mkdir -p obj
	c++ -std=c++14 -Wall -Werror -O3 -Iinclude/ $< -c -o $@

obj/arscore.o: src/arscore.cc include/w65c02.hh

gen/optable.csv: util/header_to_optable.lua
	@mkdir -p gen
	$<

tmp/%.65c.o: tests/%.65c
	@mkdir -p $(dir $@)
	wla-65c02 -q -o "$@" "$<"

tmp/%.65c.bin: tmp/%.65c.o
	@echo "[objects]" > tmp/$*.65c.link
	@echo "$<" >> tmp/$*.65c.link
	wlalink "tmp/$*.65c.link" "$@"

tmp/%.job.json: tmp/%.65c.bin
	util/makejob.lua "tests/$*.job.tmpl" "$<" "$@"

tmp/%.hw.json: tmp/%.job.json
	wget -nv -O "$@" --post-file "$<" "${CGI_URL}" || (rm -f "$@"; false)

tmp/%.ars.json: tmp/%.job.json bin/arscore
	bin/arscore < "$<" > "$@" || (rm -f "$@"; false)

tmp/%.test: util/checktest.lua tmp/%.hw.json tmp/%.ars.json
	util/checktest.lua real "tmp/$*.hw.json" ARS-emu "tmp/$*.ars.json" >"$@" 2>&1 || true

clean:
	rm -rf tmp obj bin

superclean:
	rm -rf gen tests/gen tmp obj bin

MAKEFLAGS += --no-builtin-rules
.PHONY: all clean superclean test
.SECONDARY:
