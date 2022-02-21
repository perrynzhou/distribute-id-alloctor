test:
	rm -rf core*
	rm -rf test_kv_db
	gcc -w  -g -O0 hashfn.h hashfn.c dict.h dict.c kv_db.h kv_db.c kv_db_test.c  -o test_kv_db -lwiredtiger
	rm -rf test_options
	gcc -DTEST -std=gnu99 -g  -O0  options.h options.c  -o test_options