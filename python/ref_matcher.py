"""Deliberately simple price-time priority matcher used as the M5 test oracle.

Resting orders are one flat list; the best opposite order is found by sorting
every time. It reads and writes the text protocol in include/me/order_protocol.hpp
and applies the rules documented in docs/matching.md.
"""

import sys
from dataclasses import dataclass
from itertools import count


@dataclass
class Resting:
    id: int
    side: str  # "B" or "S"
    price: int
    qty: int
    seq: int  # arrival order, for time priority


class Matcher:
    def __init__(self):
        self.resting: list[Resting] = []
        self.sequence = count()

    def _find(self, order_id):
        return next((o for o in self.resting if o.id == order_id), None)

    def _best_opposite(self, side):
        others = [o for o in self.resting if o.side != side]
        if not others:
            return None
        if side == "B":  # buyer takes the lowest ask, oldest first
            return min(others, key=lambda o: (o.price, o.seq))
        return min(others, key=lambda o: (-o.price, o.seq))

    def _enter(self, order_id, side, price, qty, kind, out):
        while qty > 0:
            best = self._best_opposite(side)
            if best is None:
                break
            if kind != "M" and (price < best.price if side == "B" else price > best.price):
                break
            fill = min(qty, best.qty)
            out.append(f"T {order_id} {best.id} {best.price} {fill}")
            qty -= fill
            best.qty -= fill
            if best.qty == 0:
                self.resting.remove(best)
        if qty == 0:
            return
        if kind == "L":
            self.resting.append(Resting(order_id, side, price, qty, next(self.sequence)))
        else:
            out.append(f"X {order_id} {price} {qty}")

    def new(self, order_id, side, price, qty, kind, out):
        if qty == 0:
            out.append(f"R {order_id} invalid_quantity")
        elif kind != "M" and price == 0:
            out.append(f"R {order_id} invalid_price")
        elif self._find(order_id):
            out.append(f"R {order_id} duplicate_id")
        elif kind == "M" and self._best_opposite(side) is None:
            out.append(f"R {order_id} no_liquidity")
        else:
            out.append(f"A {order_id} {price} {qty}")
            self._enter(order_id, side, price, qty, kind, out)

    def cancel(self, order_id, out):
        order = self._find(order_id)
        if order is None:
            out.append(f"R {order_id} unknown_order")
            return
        self.resting.remove(order)
        out.append(f"X {order_id} {order.price} {order.qty}")

    def modify(self, order_id, price, qty, out):
        order = self._find(order_id)
        if order is None:
            out.append(f"R {order_id} unknown_order")
        elif qty == 0:
            out.append(f"R {order_id} invalid_quantity")
        elif price == 0:
            out.append(f"R {order_id} invalid_price")
        else:
            out.append(f"A {order_id} {price} {qty}")
            if price == order.price and qty <= order.qty:
                order.qty = qty  # reduce in place: time priority kept
            else:
                self.resting.remove(order)  # cancel and re-add: priority lost
                self._enter(order_id, order.side, price, qty, "L", out)


def run(lines):
    """Process protocol input lines; return output lines."""
    matcher = Matcher()
    out = []
    for line in lines:
        fields = line.split()
        if not fields:
            continue
        kind, args = fields[0], fields[1:]
        if kind == "S":
            matcher = Matcher()
            out.append("S")
        elif kind == "N":
            matcher.new(int(args[0]), args[1], int(args[2]), int(args[3]), args[4], out)
        elif kind == "C":
            matcher.cancel(int(args[0]), out)
        elif kind == "M":
            matcher.modify(int(args[0]), int(args[1]), int(args[2]), out)
        else:
            raise ValueError(f"bad input line: {line!r}")
    return out


if __name__ == "__main__":
    sys.stdout.write("".join(f"{line}\n" for line in run(sys.stdin)))
