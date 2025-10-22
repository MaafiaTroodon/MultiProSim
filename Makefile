#########################################################################
# No need to change these
#########################################################################
TARGET=prosim

#########################################################################
# All C files should be added below separated by spaces.
#########################################################################
SRC_FILES=prosim.c

all: $(TARGET)

$(TARGET): $(SRC_FILES)
	gcc -Wall -g -o $(TARGET) $(SRC_FILES) -lpthread
