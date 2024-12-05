def generate_big_txt(block_size=512, num_blocks=16, output_file="big.txt"):
    with open(output_file, "w") as f:
        for i in range(num_blocks):
            letter = chr(ord('A') + (i % 26))
            block_data = letter * block_size
            f.write(block_data)

generate_big_txt()
 