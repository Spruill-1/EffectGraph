#pragma once

#include "pch_engine.h"

#include <span>

namespace ShaderLab::Graph
{
    // One rect of a region, in its node's texel space (top-left origin,
    // right and bottom exclusive), with the realize generation it was
    // ingested in. A rect merged from several keeps the oldest generation.
    struct DirtyRect
    {
        D2D1_RECT_L rect{};
        uint64_t generation{ 0 };

        bool operator==(const DirtyRect& other) const
        {
            return rect.left == other.rect.left && rect.top == other.rect.top && rect.right == other.rect.right &&
                   rect.bottom == other.rect.bottom && generation == other.generation;
        }
    };

    // A move rect carried as a move: the pixels at `source`, the size of
    // `destination`, moved there (IEffectGraphInput2::MoveRects). Moves keep
    // the order they were ingested in (`sequence`, unique per graph), so a
    // region reached twice holds each move once.
    struct DirtyMove
    {
        D2D1_RECT_L destination{};
        D2D1_POINT_2L source{};
        uint64_t generation{ 0 };
        uint64_t sequence{ 0 };

        D2D1_RECT_L Source() const
        {
            return { source.x, source.y, source.x + (destination.right - destination.left),
                     source.y + (destination.bottom - destination.top) };
        }
        bool operator==(const DirtyMove& other) const
        {
            return sequence == other.sequence && generation == other.generation && source.x == other.source.x &&
                   source.y == other.source.y && destination.left == other.destination.left &&
                   destination.top == other.destination.top && destination.right == other.destination.right &&
                   destination.bottom == other.destination.bottom;
        }
    };

    // What changed on a node: nothing, some rects, or everything. Rects never
    // number more than cMaxRects; one more collapses them all to their
    // bounding box. Every operation over-approximates, never under.
    //
    // A region may also hold moves (a Rects region with moves and no rects
    // is still dirty): its pixels are the previous image's moved, before the
    // rects are drawn, in desktop duplication's order. Whatever cannot carry
    // a move turns it into a rect at its destination (CollapseMoves), which
    // is always correct; Whole drops them.
    class DirtyRegion
    {
    public:
        enum class Kind : uint8_t
        {
            None,
            Rects,
            Whole,
        };

        static constexpr size_t cMaxRects = 16;
        static constexpr size_t cMaxMoves = 16;

        // The generation a collapsed move's destination is tagged with: older
        // than the move, so a later move whose source reads it collapses too.
        static uint64_t CollapsedGeneration(uint64_t generation) { return generation ? generation - 1 : 0; }

        DirtyRegion() = default;

        static DirtyRegion Whole()
        {
            DirtyRegion region;
            region.m_kind = Kind::Whole;
            return region;
        }

        static DirtyRegion Rect(const D2D1_RECT_L& rect, uint64_t generation)
        {
            DirtyRegion region;
            region.Add(rect, generation);
            return region;
        }

        Kind GetKind() const { return m_kind; }
        bool IsNone() const { return m_kind == Kind::None; }
        bool IsWhole() const { return m_kind == Kind::Whole; }
        bool IsRects() const { return m_kind == Kind::Rects; }
        std::span<const DirtyRect> Rects() const { return { m_rects.data(), m_count }; }
        std::span<const DirtyMove> Moves() const { return m_moves; }
        bool HasMoves() const { return !m_moves.empty(); }

        // Adds a move after those held, unless one with its sequence is held
        // already. Whole ignores it; an empty one changes nothing. Past
        // cMaxMoves every move becomes a rect.
        void AddMove(const DirtyMove& move)
        {
            if (m_kind == Kind::Whole || IsEmpty(move.destination)) return;
            m_kind = Kind::Rects;
            const auto at = std::ranges::lower_bound(m_moves, move.sequence, {}, &DirtyMove::sequence);
            if (at != m_moves.end() && at->sequence == move.sequence) return;
            m_moves.insert(at, move);
            if (m_moves.size() > cMaxMoves) CollapseMoves();
        }

        // Every move becomes a rect at its destination, tagged older than it.
        void CollapseMoves()
        {
            auto moves = std::move(m_moves);
            m_moves.clear();
            for (const auto& move : moves)
                Add(move.destination, CollapsedGeneration(move.generation));
        }

        DirtyRegion WithMovesCollapsed() const
        {
            DirtyRegion collapsed = *this;
            collapsed.CollapseMoves();
            return collapsed;
        }

        static bool IsEmpty(const D2D1_RECT_L& rect) { return rect.right <= rect.left || rect.bottom <= rect.top; }

        static bool Contains(const D2D1_RECT_L& outer, const D2D1_RECT_L& inner)
        {
            return outer.left <= inner.left && outer.top <= inner.top && outer.right >= inner.right &&
                   outer.bottom >= inner.bottom;
        }

        static D2D1_RECT_L Intersect(const D2D1_RECT_L& a, const D2D1_RECT_L& b)
        {
            return { (std::max)(a.left, b.left), (std::max)(a.top, b.top), (std::min)(a.right, b.right),
                     (std::min)(a.bottom, b.bottom) };
        }

        // Adds a rect. An empty one changes nothing; one inside a rect already
        // held is dropped, and rects inside it are replaced by it.
        void Add(const D2D1_RECT_L& rect, uint64_t generation)
        {
            if (m_kind == Kind::Whole || IsEmpty(rect)) return;
            m_kind = Kind::Rects;
            for (size_t i = 0; i < m_count; ++i)
                if (Contains(m_rects[i].rect, rect))
                {
                    m_rects[i].generation = (std::min)(m_rects[i].generation, generation);
                    return;
                }
            size_t kept = 0;
            for (size_t i = 0; i < m_count; ++i)
            {
                if (Contains(rect, m_rects[i].rect))
                {
                    generation = (std::min)(generation, m_rects[i].generation);
                    continue;
                }
                m_rects[kept++] = m_rects[i];
            }
            m_count = static_cast<uint8_t>(kept);
            if (m_count == cMaxRects)
            {
                // The bounding box, tagged with the oldest part.
                D2D1_RECT_L box = rect;
                uint64_t oldest = generation;
                for (size_t i = 0; i < m_count; ++i)
                {
                    box = Bound(box, m_rects[i].rect);
                    oldest = (std::min)(oldest, m_rects[i].generation);
                }
                m_rects[0] = { box, oldest };
                m_count = 1;
                return;
            }
            m_rects[m_count++] = { rect, generation };
        }

        void Union(const DirtyRegion& other)
        {
            if (m_kind == Kind::Whole || other.m_kind == Kind::None) return;
            if (other.m_kind == Kind::Whole)
            {
                *this = Whole();
                return;
            }
            for (const auto& rect : other.Rects())
                Add(rect.rect, rect.generation);
            for (const auto& move : other.Moves())
                AddMove(move);
        }

        // The rects clipped to `bounds`, empties dropped. Whole stays Whole.
        // A move whose source or destination leaves `bounds` becomes a rect.
        DirtyRegion ClippedTo(const D2D1_RECT_L& bounds) const
        {
            if (m_kind != Kind::Rects) return *this;
            DirtyRegion clipped;
            for (const auto& rect : Rects())
                clipped.Add(Intersect(rect.rect, bounds), rect.generation);
            for (const auto& move : Moves())
            {
                if (Contains(bounds, move.destination) && Contains(bounds, move.Source()))
                    clipped.AddMove(move);
                else
                    clipped.Add(Intersect(move.destination, bounds), CollapsedGeneration(move.generation));
            }
            return clipped;
        }

        // The smallest rect holding every rect; empty for None and Whole.
        D2D1_RECT_L Bounds() const
        {
            if (m_kind != Kind::Rects || m_count == 0) return {};
            D2D1_RECT_L box = m_rects[0].rect;
            for (size_t i = 1; i < m_count; ++i)
                box = Bound(box, m_rects[i].rect);
            return box;
        }

        // The oldest generation any rect carries; UINT64_MAX for None and Whole.
        uint64_t OldestGeneration() const
        {
            uint64_t oldest = UINT64_MAX;
            for (const auto& rect : Rects())
                oldest = (std::min)(oldest, rect.generation);
            return oldest;
        }

        // Pixels the rects cover, overlaps counted once.
        uint64_t UnionArea() const
        {
            if (m_kind != Kind::Rects) return 0;
            std::vector<LONG> xs, ys;
            for (const auto& rect : Rects())
            {
                xs.push_back(rect.rect.left); xs.push_back(rect.rect.right);
                ys.push_back(rect.rect.top); ys.push_back(rect.rect.bottom);
            }
            std::sort(xs.begin(), xs.end());
            xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
            std::sort(ys.begin(), ys.end());
            ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
            uint64_t area = 0;
            for (size_t i = 0; i + 1 < xs.size(); ++i)
                for (size_t j = 0; j + 1 < ys.size(); ++j)
                {
                    const D2D1_RECT_L cell{ xs[i], ys[j], xs[i + 1], ys[j + 1] };
                    for (const auto& rect : Rects())
                        if (Contains(rect.rect, cell))
                        {
                            area += static_cast<uint64_t>(cell.right - cell.left) * static_cast<uint64_t>(cell.bottom - cell.top);
                            break;
                        }
                }
            return area;
        }

        // True when the rects, together, cover all of `bounds`.
        bool Covers(const D2D1_RECT_L& bounds) const
        {
            if (m_kind == Kind::Whole) return true;
            if (m_kind == Kind::None || IsEmpty(bounds)) return false;
            const uint64_t boundsArea =
                static_cast<uint64_t>(bounds.right - bounds.left) * static_cast<uint64_t>(bounds.bottom - bounds.top);
            return ClippedTo(bounds).UnionArea() == boundsArea;
        }

        bool operator==(const DirtyRegion& other) const
        {
            if (m_kind != other.m_kind || m_count != other.m_count) return false;
            for (size_t i = 0; i < m_count; ++i)
                if (!(m_rects[i] == other.m_rects[i])) return false;
            return m_moves == other.m_moves;
        }

    private:
        static D2D1_RECT_L Bound(const D2D1_RECT_L& a, const D2D1_RECT_L& b)
        {
            return { (std::min)(a.left, b.left), (std::min)(a.top, b.top), (std::max)(a.right, b.right),
                     (std::max)(a.bottom, b.bottom) };
        }

        Kind m_kind{ Kind::None };
        uint8_t m_count{ 0 };
        std::array<DirtyRect, cMaxRects> m_rects{};
        std::vector<DirtyMove> m_moves;
    };

    // What an incremental draw does with a root's pending moves, in order:
    // a move whose source a still-pending older rect overlaps would copy
    // pixels the target never got, and a move of generation `carryThrough`
    // or older was pending during this root's last realize, which failed or
    // did not draw (a carry-over): both collapse to a rect at the
    // destination, older than every later move. A rect of the move's own
    // generation never collapses it, since rects are drawn after the moves
    // (MoveRects advances the generation when anything was ingested before
    // it in the current one). `apply` false collapses them all.
    struct MovePlan
    {
        std::vector<DirtyMove> kept;   // applied as copies, in order
        DirtyRegion rects;             // then drawn: the region's rects and every collapsed destination
        uint32_t collapsed{ 0 };
    };

    // `collapse`: sequences (sorted) that collapse whatever else holds.
    inline MovePlan PlanMoves(const DirtyRegion& region, uint64_t carryThrough, bool apply,
                              std::span<const uint64_t> collapse = {})
    {
        MovePlan plan;
        if (!region.IsRects())
        {
            plan.rects = region;
            return plan;
        }
        for (const auto& rect : region.Rects())
            plan.rects.Add(rect.rect, rect.generation);
        for (const auto& move : region.Moves())
        {
            bool keep = apply && move.generation > carryThrough &&
                        !std::ranges::binary_search(collapse, move.sequence);
            const D2D1_RECT_L source = move.Source();
            for (const auto& rect : plan.rects.Rects())
            {
                if (!keep) break;
                if (rect.generation < move.generation && !DirtyRegion::IsEmpty(DirtyRegion::Intersect(rect.rect, source)))
                    keep = false;
            }
            if (keep)
            {
                plan.kept.push_back(move);
                continue;
            }
            plan.rects.Add(move.destination, DirtyRegion::CollapsedGeneration(move.generation));
            ++plan.collapsed;
        }
        return plan;
    }
}
