import sys
def generate_big_txt(num_blocks=16, output_file="big.txt"):
    block_size = 512
    with open(output_file, "w") as f:
        for i in range(num_blocks):
            letter = chr(ord('A') + (i % 26))
            block_data = letter * block_size
            f.write(block_data)

generate_big_txt(int(sys.argv[1]), sys.argv[2])
 