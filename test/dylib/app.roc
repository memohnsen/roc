app [main!] { pf: platform "./platform/main.roc" }

import pf.Host

# A top-level constant that holds pointers: the list points at its elements,
# and each string, too long to be stored inline, points at its bytes. A shared
# library must relocate these pointers when it is loaded.
labels : List(Str)
labels = ["a label that is too long to be stored inline", "another label that is too long to be stored inline"]

main! : I64 => I64
main! = |n| {
    # A runtime list literal allocates, exercising the host's exported roc_alloc.
    list = [n, n, n]
    total = list.fold(0, |acc, elem| acc + elem)
    # Reads the constant through both of its pointers. One label matches.
    matching = labels.count_if(|label| label.starts_with("another"))
    Host.double!(total) + matching.to_i64_wrap()
}
