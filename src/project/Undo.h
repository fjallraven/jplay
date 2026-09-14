#pragma once

#include <functional>
#include <string>
#include <vector>

// A single reversible edit. revert() restores the pre-edit state; apply() redoes
// it. Both re-resolve the objects they touch by id (never by pointer) so captured
// state can't dangle across later edits. label is shown in the status line.
struct UndoCommand {
    std::string label;
    std::function<void()> revert; // undo: model -> pre-edit state
    std::function<void()> apply;  // redo: model -> post-edit state
};

// A plain undo/redo stack of UndoCommands. Pushing a new command clears the redo
// stack (the usual linear-history behaviour). undo()/redo() return the label of
// the action performed, or an empty string when there is nothing to do.
class UndoStack {
public:
    void push(UndoCommand cmd) {
        undo_.push_back(std::move(cmd));
        redo_.clear();
    }

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }

    std::string undo() {
        if (undo_.empty()) return {};
        UndoCommand c = std::move(undo_.back());
        undo_.pop_back();
        c.revert();
        std::string label = c.label;
        redo_.push_back(std::move(c));
        return label;
    }

    std::string redo() {
        if (redo_.empty()) return {};
        UndoCommand c = std::move(redo_.back());
        redo_.pop_back();
        c.apply();
        std::string label = c.label;
        undo_.push_back(std::move(c));
        return label;
    }

    void clear() { undo_.clear(); redo_.clear(); }

private:
    std::vector<UndoCommand> undo_;
    std::vector<UndoCommand> redo_;
};
