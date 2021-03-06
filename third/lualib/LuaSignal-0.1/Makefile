include Make.config

BIN = src/signal.so
OBJ = src/signal.o \
      src/signames.o \
      src/queue.o
CC = gcc
DEFINES =
LIBS = 
LUAINC = -I../lua-5.2.1/src
# COMMONFLAGS = -fpic -Werror -Wall -pedantic -O0 -g -pipe
COMMONFLAGS = -fpic -Werror -Wall -pedantic -O2 -g -pipe
CFLAGS = -c $(LUAINC) $(DEFINES) $(COMMONFLAGS) -DLUA_COMPAT_MODULE
LDFLAGS = -shared $(LIBS) $(COMMONFLAGS)

build : $(BIN)

$(BIN) : $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) -o $@

%.o : %.c
	$(CC) $(CFLAGS) -o $@ $<

install : $(BIN)
	mkdir -p $(LUA_DIR)
	cp $(BIN) $(LUA_DIR)

MAIN_SRC = src/signal.c
DOC_SRC = src/signal.luadoc
OTHER_SRC = src/signames.c src/signames.h src/queue.c src/queue.h
DOC_DIR = doc

doc :
	mkdir -p $(DOC_DIR)
	$(LUADOC) --nofiles -d $(DOC_DIR) $(DOC_SRC)
	@touch doc

clean :
	rm -rf $(OBJ) $(BIN) $(DOC_DIR)

TEST_SRC = test/signal_test.lua test/alarm_test.lua test/simple_test.lua
OTHER_FILES = Makefile Make.config README LICENSE TODO
VERSION = $(shell grep 'define VERSION ' $(MAIN_SRC) | sed 's/.define VERSION "\(.*\)"/\1/' | tr ' ' '-')

dist : $(VERSION).tar.gz

$(VERSION).tar.gz : $(MAIN_SRC) $(OTHER_SRC) $(TEST_SRC) $(DOC_SRC) $(DOC_DIR) $(OTHER_FILES)
	@echo "Creating $(VERSION).tar.gz"
	@mkdir $(VERSION)
	@mkdir $(VERSION)/src
	@cp $(MAIN_SRC) $(DOC_SRC) $(OTHER_SRC) $(VERSION)/src
	@mkdir $(VERSION)/test
	@cp $(TEST_SRC) $(VERSION)/test
	@mkdir $(VERSION)/doc
	@cp -r $(DOC_DIR)/* $(VERSION)/doc
	@cp $(OTHER_FILES) $(VERSION)
	@tar czf $(VERSION).tar.gz $(VERSION)
	@rm -rf $(VERSION)

dep :
	makedepend $(LUAINC) $(DEFINES) -Y src/*.c src/*.h > /dev/null 2>&1
	rm -f Makefile.bak

# DO NOT DELETE

src/queue.o: src/queue.h
src/signal.o: src/queue.h src/signames.h
src/signames.o: src/signames.h
