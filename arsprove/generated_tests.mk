test: $(patsubst tests/%.65c,tmp/%.test,$(shell find tests -name \*.65c))
	util/check_test_results.sh
