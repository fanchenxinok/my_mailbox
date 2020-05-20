CC = gcc
SOURCES = $(wildcard ./*.c)
INCLUDE_DIRS = -I./
TARGET = queue_fw_test
OBJECTS = $(patsubst %.c,%.o,$(SOURCES))
CLIBS = -lpthread
CFLAGS = -o0 -g

$(TARGET) : $(OBJECTS)
	$(CC) $^ -o $@ $(CLIBS)
	
$(OBJECTS) : %.o : %.c 
	$(CC) -c $(CFLAGS) $< -o $@ $(INCLUDE_DIRS)

.PHONY : clean
clean:
	rm -rf $(TARGET) $(OBJECTS)