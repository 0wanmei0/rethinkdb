#ifndef __FSCK_CHECKER_HPP__
#define __FSCK_CHECKER_HPP__

#include <vector>

#include "utils.hpp"

namespace fsck {

/*
  Here are the checks that we perform.

  - that block_size divides extent_size divides filesize.  (size ratio
    check)

  - that all keys are in the appropriate slice.  (hash check)

  - that all keys in the correct order.  (order check)

  - that the tree is properly balanced.  (balance check)

  - that there are no disconnected nodes.  (connectedness check)

  - that deleted blocks have proper zerobufs.  (zeroblock check)

  - that large bufs have all the proper blocks.  (large buffer check)

  - that keys and small values are no longer than MAX_KEY_SIZE and
    MAX_IN_NODE_VALUE_SIZE, and that large values are no longer than
    MAX_VALUE_SIZE and longer than MAX_IN_NODE_VALUE_SIZE.  (size
    limit check)

  - that used block ids (including deleted block ids) within each
    slice are contiguous.  (contiguity check)

  - that all metablock CRCs match, except for zeroed out blocks.
    (metablock crc check)

  - that the metablock version numbers and serializer transaction ids
    grow monotonically from some starting point.  (metablock
    monotonicity check)

  - that blocks in use have greater transaction ids than blocks not
    in use.  (supremacy check)

  - that the patches in the diff storage are sequentially numbered
*/

struct config_t {
    std::vector<std::string> input_filenames;
    std::string metadata_filename;
    std::string log_file_name;
    bool ignore_diff_log;

    bool print_command_line;
    bool print_file_version;

    config_t() : ignore_diff_log(false), print_command_line(false), print_file_version(false) {}
};

bool check_files(const config_t *config);

std::string extract_command_line_args(const config_t *cfg);



}  // namespace fsck



#endif  // __FSCK_CHECKER_HPP__
